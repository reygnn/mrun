/*
 * mrun - a minimal run dialog for Wayland, in the Motif look of mwc
 * (https://github.com/reygnn/mwc) and mfm.
 *
 * One file on mtk (mtk/), the toolkit of mwc's clients, which brings the window,
 * the seat, the keys, the cursor and the colours. It lists the programs in $PATH,
 * once at the start, and narrows them down while typing: exact names first, then
 * those starting with the text, then those containing it, ignoring case. It reads
 * the same colours as mwc: MWC_INACTIVE / MWC_ACTIVE from the environment, else
 * Mwm*background and Mwm*activeBackground from $XDG_CONFIG_HOME/mwc/mwmrc
 * (~/.config/mwc/mwmrc), else the Motif defaults; the text in Mwm*foreground.
 *
 * Keys:   typing  narrows the list      Up/Down/PgUp/PgDn  choose
 *         Return  run                   Tab  take the chosen name
 *         Left/Right/Home/End/Backspace/Delete  edit    Ctrl+U  clear
 *         Esc / Ctrl+Q / Ctrl+W  quit
 * Mouse:  click chooses, double click runs, wheel scrolls.
 *
 * Without a blank, Return runs the chosen program directly. With a blank, the line
 * goes to the shell as it is, so it can have arguments and shell syntax. A blank
 * in single quotes or after a backslash is part of the name, as for the shell. Nothing
 * runs in a terminal, and what a program prints goes where mrun's output goes.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "mtk.h"
#include "version.h"

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define DCLICK_MS 400
#ifndef MAX            /* glib has them too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct rect { int x, y, w, h; };

/* ---- state -------------------------------------------------------------- */

struct prog {
	char *path;             /* as found in $PATH, what is started */
	const char *name;       /* in path */
	char *fold;             /* the name case folded, to match and sort by */
	int order;              /* of finding it: the first in $PATH wins */
};

static struct prog *progs;      /* sorted by name, no duplicates */
static int nnames;
static int *matches;            /* indexes into progs, best first */
static int nmatches;
static int sel = -1, top;       /* chosen and first shown of the matches */
static char line[4096];
static size_t cur;              /* byte offset of the text cursor */
static char status_msg[256];
static uint32_t last_click_ms;
static int last_click_row = -1;
static const char *test_state;  /* MRUN_TEST_STATE: where the tests read it */
/* For the tests: F6 presses, to wait for all input before one; directories read. */
static unsigned syncs, dirs_read;

static struct mtk *m;
static struct mtk_window *win;
static int ptr_x, ptr_y;

static int W = 420, H = 320;
static struct mtk_theme theme;
static uint32_t px_bg, px_top, px_bot, px_list, px_paper, px_sel;
static uint32_t xc_fg, xc_sel, xc_dim, xc_err;
static cairo_t *cr;             /* what fill, bevel and text draw on */
static PangoLayout *plo;
static int font_asc, font_desc;
static int rowh, barh;
static struct rect r_input, r_list, r_status;

static void redraw(void)
{
	if (win)
		mtk_window_redraw(win);
}

/* ---- the programs in $PATH ---------------------------------------------- */

/* Case only as a tie break, so the same name lands next to itself. */
static int prog_cmp(const void *a, const void *b)
{
	const struct prog *p = a, *q = b;
	int c = strcmp(p->fold, q->fold);
	if (!c)
		c = strcmp(p->name, q->name);
	return c ? c : p->order - q->order;
}

/* What can't be typed or drawn isn't offered: not valid UTF-8, or control characters. */
static int name_usable(const char *s)
{
	for (const char *p = s; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f)
			return 0;
	return g_utf8_validate(s, -1, NULL);
}

static void scan_path(void)
{
	const char *path = getenv("PATH");
	int cap = 0;
	/* The directories read: a merged /usr or a profile that adds an entry again
	 * names one twice, and its programs would only be found twice. */
	struct { dev_t dev; ino_t ino; } seen[64];      /* past that only slower */
	int nseen = 0;
	char *copy = strdup(path ? path : "");
	if (!copy) {
		fprintf(stderr, "mrun: out of memory\n");
		exit(1);
	}
	for (char *save, *dir = strtok_r(copy, ":", &save); dir;
	     dir = strtok_r(NULL, ":", &save)) {
		/* A relative entry would mean whatever directory we were started in. */
		if (dir[0] != '/')
			continue;
		int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
		if (!d) {
			if (fd >= 0)
				close(fd);
			continue;
		}
		struct stat ds;
		int again = 0;
		if (fstat(fd, &ds) == 0) {
			for (int i = 0; i < nseen && !again; i++)
				again = seen[i].dev == ds.st_dev && seen[i].ino == ds.st_ino;
			if (!again && nseen < (int)(sizeof seen / sizeof *seen)) {
				seen[nseen].dev = ds.st_dev;
				seen[nseen++].ino = ds.st_ino;
			}
		}
		if (again) {
			closedir(d);
			continue;
		}
		dirs_read++;
		struct dirent *e;
		while ((e = readdir(d))) {
			struct stat st;
			if (e->d_name[0] == '.' || !name_usable(e->d_name))
				continue;
			if (fstatat(fd, e->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode) ||
			    faccessat(fd, e->d_name, X_OK, 0) != 0)
				continue;
			if (nnames == cap) {
				cap = cap ? cap * 2 : 1024;
				struct prog *grown = realloc(progs, cap * sizeof *progs);
				if (!grown) {
					fprintf(stderr, "mrun: out of memory\n");
					exit(1);
				}
				progs = grown;
			}
			struct prog *pr = &progs[nnames];
			size_t dl = strlen(dir);
			if (!(pr->path = malloc(dl + strlen(e->d_name) + 2))) {
				fprintf(stderr, "mrun: out of memory\n");
				exit(1);
			}
			sprintf(pr->path, "%s/%s", dir, e->d_name);
			pr->name = pr->path + dl + 1;
			pr->fold = g_utf8_casefold(pr->name, -1);
			pr->order = nnames++;
		}
		closedir(d);
	}
	free(copy);
	if (nnames)
		qsort(progs, nnames, sizeof *progs, prog_cmp);
	/* The same name twice: the shell would only ever find the first. */
	int kept = 0;
	for (int i = 0; i < nnames; i++) {
		if (kept && !strcmp(progs[kept - 1].name, progs[i].name)) {
			free(progs[i].path);
			g_free(progs[i].fold);
		} else {
			progs[kept++] = progs[i];
		}
	}
	nnames = kept;
	if (!(matches = malloc((size_t)(nnames + 1) * sizeof *matches))) {
		fprintf(stderr, "mrun: out of memory\n");
		exit(1);
	}
}

/* The first word of the line as the shell splits it, quoted as shell_word quotes:
 * its length, and in out (if not NULL) its text without the quotes. */
static size_t first_word_text(char *out)
{
	size_t i = 0, o = 0;
	int quoted = 0;
	for (; line[i] && (quoted || (line[i] != ' ' && line[i] != '\t')); i++) {
		if (line[i] == '\'') {
			quoted = !quoted;
			continue;
		}
		if (!quoted && line[i] == '\\' && line[i + 1])
			i++;            /* the '\'' in a quoted quote */
		if (out)
			out[o++] = line[i];
	}
	if (out)
		out[o] = 0;
	return i;
}

static size_t first_word(void)
{
	return first_word_text(NULL);
}

static int only_blanks(void)
{
	return !line[strspn(line, " \t")];
}

/* Blanks alone are as good as nothing typed. */
static int has_args(void)
{
	return line[first_word()] != 0 && !only_blanks();
}

/* The status line, cut on a character: Pango wants valid UTF-8. */
static void __attribute__((format(printf, 1, 2))) set_status(const char *fmt, ...)
{
	va_list ap;
	const char *end;
	va_start(ap, fmt);
	vsnprintf(status_msg, sizeof status_msg, fmt, ap);
	va_end(ap);
	g_utf8_validate(status_msg, -1, &end);
	status_msg[end - status_msg] = 0;
	redraw();
}

static int rows_vis(void)
{
	return MAX((r_list.h - 2 * BEV - 4) / rowh, 1);
}

static void show_sel(void)
{
	if (sel < 0)
		top = 0;
	else if (sel < top)
		top = sel;
	else if (sel >= top + rows_vis())
		top = sel - rows_vis() + 1;
	top = MAX(MIN(top, nmatches - rows_vis()), 0);
}

/* Best first, and otherwise in the order of the names. */
static void update_matches(void)
{
	static char q[sizeof line];
	static int have_q;
	char word[sizeof line];
	first_word_text(word);
	status_msg[0] = 0;
	redraw();
	/* Typing the arguments keeps the list, and what is chosen in it; so does Tab,
	 * which only quotes the name. */
	if (have_q && !strcmp(q, word))
		return;
	strcpy(q, word);
	have_q = 1;
	last_click_row = -1;    /* a click on another list is no double click */
	nmatches = 0;
	char *fq = g_utf8_casefold(q, -1);
	size_t fl = strlen(fq);
	/* The exact name as typed goes before the same in another case. */
	for (int rank = 0; rank < 4; rank++) {
		for (int i = 0; i < nnames; i++) {
			const char *f = progs[i].fold;
			int is_prefix = !strncmp(f, fq, fl);
			int is_exact = is_prefix && !f[fl];
			int as_typed = is_exact && !strcmp(progs[i].name, q);
			if (rank == 0 ? as_typed :
			    rank == 1 ? is_exact && !as_typed :
			    rank == 2 ? is_prefix && !is_exact :
			    !is_prefix && strstr(f, fq))
				matches[nmatches++] = i;
		}
	}
	g_free(fq);
	/* With nothing typed nothing is chosen, so Return doesn't run the first of all. */
	sel = q[0] && nmatches ? 0 : -1;
	top = 0;
}

/* ---- running ------------------------------------------------------------ */

static void cannot_start(int err)
{
	set_status("cannot start: %s", strerror(err));
}

/*
 * Starts it in a session of its own and ends here, unless it can't be started. A failed
 * exec comes back through the pipe, which closes by itself when the exec succeeds.
 */
static void run_argv(const char *file, char *const argv[])
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		cannot_start(errno);
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		cannot_start(errno);
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		close(fds[0]);
		setsid();
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0) {
			dup2(null, 0);
			if (null != 0)
				close(null);
		}
		execvp(file, argv);
		int err = errno;
		/* The pipe first: a stderr nobody reads any more would end it here. */
		while (write(fds[1], &err, sizeof err) < 0 && errno == EINTR)
			;
		fprintf(stderr, "mrun: %s: %s\n", file, strerror(err));
		_exit(127);
	}
	close(fds[1]);
	int err;
	ssize_t n;
	while ((n = read(fds[0], &err, sizeof err)) < 0 && errno == EINTR)
		;
	close(fds[0]);
	if (n != sizeof err)
		exit(0);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
	cannot_start(err);
}

/* By the path it was found at: execvp would also search what scan_path skips. */
static void run_prog(const struct prog *p)
{
	char *const argv[] = { (char *)p->name, NULL };
	run_argv(p->path, argv);
}

static void run_line(void)
{
	/* After "--" a line starting with "-" is a command, not options. */
	char *const argv[] = { (char *)"sh", (char *)"-c", (char *)"--", line, NULL };
	run_argv("/bin/sh", argv);
}

/* What a word can have that the shell reads as it is. No "=": a name with it at
 * the start would be an assignment. */
static const char shell_plain[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"0123456789_+-.,:@%/";

/* Return. With a blank the line is what the user wants, whatever is chosen. */
static void activate(void)
{
	if (has_args())
		run_line();
	else if (sel >= 0)
		run_prog(&progs[matches[sel]]);
	else if (only_blanks())
		return;
	else if (strchr(line, '/') && !line[strspn(line, shell_plain)])
		run_argv(line, (char *[]){ line, NULL });     /* its error comes back */
	else if (strchr(line, '/'))
		run_line();
	else
		set_status("not in $PATH: %s", line);
}

/* A word the shell takes for syntax at the start of a command. */
static int reserved(const char *n)
{
	static const char *const words[] = { "case", "do", "done", "elif", "else",
		"esac", "fi", "for", "function", "if", "in", "select", "then", "time",
		"until", "while" };
	for (size_t i = 0; i < sizeof words / sizeof *words; i++)
		if (!strcmp(n, words[i]))
			return 1;
	return 0;
}

/* The name as the shell reads it: in single quotes if it has anything but
 * shell_plain, or is a reserved word. */
static size_t shell_word(const char *n, char *out, size_t size)
{
	if (!n[strspn(n, shell_plain)] && !reserved(n))
		return (size_t)snprintf(out, size, "%s", n);
	size_t o = 0;
	out[o++] = '\'';
	for (; *n && o + 5 < size; n++) {
		if (*n == '\'') {
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			out[o++] = *n;
		}
	}
	if (*n)
		return size;    /* too long */
	out[o++] = '\'';
	out[o] = 0;
	return o;
}

/* Puts the chosen name in place of the first word; what follows stays, or a blank
 * after it if nothing does. 0 if it doesn't fit, and the line is as it was. */
static int take_chosen(void)
{
	if (sel < 0)
		return 0;
	char n[sizeof line];
	size_t nl = shell_word(progs[matches[sel]].name, n, sizeof n);
	size_t old = first_word(), rest = strlen(line) - old;
	int blank = !rest;
	if (nl >= sizeof n || nl + rest + blank + 1 > sizeof line) {
		set_status("the line has no room for the name");
		return 0;
	}
	memmove(line + nl, line + old, rest + 1);
	memcpy(line, n, nl);
	if (blank) {
		line[nl] = ' ';
		line[nl + 1] = 0;
	}
	cur = nl + 1;
	update_matches();
	return 1;
}

/* ---- colours and font ---------------------------------------------------- */

static void theme_init(void)
{
	mtk_theme_load(&theme);
	uint32_t bg = theme.inactive.bg;
	/* As in mwc: the text is Mwm*foreground where set, on the chosen row too. Else
	 * the chosen row, which is mwc's armed menu item, as Motif picks it, and the
	 * list, a face of mrun's own and lighter than bg, black or white by WCAG. */
	px_bg = bg;
	px_top = theme.inactive.top;
	px_bot = theme.inactive.bot;
	px_list = mtk_mix(bg, 0xffffff, 0.3f);
	px_paper = 0xf6f6f6;
	px_sel = theme.active.bg;
	uint32_t fg = theme.inactive.fg;
	xc_fg = theme.has_fg ? fg : mtk_best_contrast(px_list);
	xc_sel = theme.has_fg ? fg : mtk_text_color(px_sel);
	xc_dim = mtk_mix(bg, fg, 0.6f);
	/* Light text means a dark theme, where dark red wouldn't read. */
	xc_err = mtk_best_contrast(fg) == 0x000000 ? 0xffa0a0 : 0x8a0303;

	const char *f = getenv("MRUN_FONT");
	PangoFontDescription *desc =
		pango_font_description_from_string(f && *f ? f : "Sans 10");
	/* "Monospace" has no size, and metrics without one are all zero. */
	if (pango_font_description_get_size(desc) <= 0)
		pango_font_description_set_size(desc, 10 * PANGO_SCALE);
	plo = mtk_layout_new(desc);
	/* Left aligned also for a name that starts right to left. */
	pango_layout_set_auto_dir(plo, FALSE);
	PangoFontMetrics *fm = pango_context_get_metrics(pango_layout_get_context(plo),
		desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(fm));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(fm));
	pango_font_metrics_unref(fm);
	pango_font_description_free(desc);
	rowh = font_asc + font_desc + 4;
	barh = rowh + 6;
}

/* ---- drawing ------------------------------------------------------------ */

static void set_rgb(uint32_t c)
{
	mtk_set_rgb(cr, c);
}

static void fill(uint32_t px, int x, int y, int w, int h)
{
	mtk_fill(cr, px, x, y, w, h);
}

static void bevel(struct rect r, int raised)
{
	mtk_bevel(cr, r.x, r.y, r.w, r.h, BEV, raised ? px_top : px_bot,
		raised ? px_bot : px_top);
}

/* s in at most maxw pixels (with "..." at the end if it is longer). */
static void text(uint32_t c, int x, int y, int h, int maxw, const char *s)
{
	if (maxw <= 0)
		return;
	int base = y + (h - (font_asc + font_desc)) / 2 + font_asc;
	pango_layout_set_text(plo, s, -1);
	pango_layout_set_width(plo, maxw * PANGO_SCALE);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_END);
	set_rgb(c);
	cairo_move_to(cr, x, base - pango_layout_get_baseline(plo) / PANGO_SCALE);
	pango_cairo_show_layout(cr, plo);
	pango_layout_set_width(plo, -1);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_NONE);
}

static void layout(void)
{
	int y0 = PAD + barh + 4;
	r_input = (struct rect){ PAD, PAD, W - 2 * PAD, barh };
	r_status = (struct rect){ PAD, H - PAD - barh, W - 2 * PAD, barh };
	r_list = (struct rect){ PAD, y0, W - 2 * PAD, r_status.y - 4 - y0 };
}

/* The line with its text cursor, shifted left if the end doesn't fit. */
static void draw_input(void)
{
	int fx = r_input.x + BEV, fy = r_input.y + BEV;
	int fw = r_input.w - 2 * BEV, fh = r_input.h - 2 * BEV;
	fill(px_paper, fx, fy, fw, fh);
	bevel(r_input, 0);
	PangoRectangle pos;
	pango_layout_set_text(plo, line, -1);
	pango_layout_index_to_pos(plo, (int)cur, &pos);
	int cx = PANGO_PIXELS(pos.x), room = fw - 8;
	int x = fx + 4 - MAX(cx + 1 - room, 0);
	int base = r_input.y + (r_input.h - (font_asc + font_desc)) / 2 + font_asc;
	cairo_save(cr);
	cairo_rectangle(cr, fx, fy, fw, fh);
	cairo_clip(cr);
	set_rgb(0x000000);
	cairo_move_to(cr, x, base - pango_layout_get_baseline(plo) / PANGO_SCALE);
	pango_cairo_show_layout(cr, plo);
	fill(0x000000, x + cx, fy + 2, 1, fh - 4);
	cairo_restore(cr);
}

static void paint(struct mtk_window *w, cairo_t *c, int width, int height, void *data)
{
	char s[sizeof status_msg];
	cr = c;
	W = width;
	H = height;
	/* Glyph advances at the buffer's pixels, not rounded to the surface's. */
	pango_cairo_update_context(cr, pango_layout_get_context(plo));
	pango_layout_context_changed(plo);
	layout();
	fill(px_bg, 0, 0, W, H);
	draw_input();

	int lx = r_list.x + BEV, ly = r_list.y + BEV + 2;
	int lw = r_list.w - 2 * BEV, vis = rows_vis();
	fill(px_list, lx, r_list.y + BEV, lw, r_list.h - 2 * BEV);
	bevel(r_list, 0);
	cairo_save(cr);
	cairo_rectangle(cr, lx, r_list.y + BEV, lw, r_list.h - 2 * BEV);
	cairo_clip(cr);
	for (int i = top; i < nmatches && i < top + vis; i++) {
		int y = ly + (i - top) * rowh;
		if (i == sel) {
			/* As mwc arms a menu item: the active colour, raised. */
			struct rect r = { lx + 1, y, lw - 2, rowh };
			fill(px_sel, r.x, r.y, r.w, r.h);
			bevel(r, 1);
		}
		text(i == sel ? xc_sel : xc_fg, lx + 5, y, rowh, lw - 10,
			progs[matches[i]].name);
	}
	cairo_restore(cr);

	bevel(r_status, 0);
	int sx = r_status.x + BEV + 4, sw = r_status.w - 2 * BEV - 8;
	cairo_save(cr);
	cairo_rectangle(cr, r_status.x + BEV, r_status.y + BEV, r_status.w - 2 * BEV,
		r_status.h - 2 * BEV);
	cairo_clip(cr);
	if (status_msg[0]) {
		text(xc_err, sx, r_status.y, r_status.h, sw, status_msg);
	} else {
		if (has_args())
			snprintf(s, sizeof s, "Return runs the line with the shell");
		else if (line[0])
			snprintf(s, sizeof s, "%d of %d programs", nmatches, nnames);
		else
			snprintf(s, sizeof s, "%d programs", nnames);
		text(xc_dim, sx, r_status.y, r_status.h, sw, s);
	}
	cairo_restore(cr);
}

/* ---- input -------------------------------------------------------------- */

static size_t prev_char(size_t at)
{
	while (at > 0 && ((unsigned char)line[--at] & 0xc0) == 0x80)
		;
	return at;
}

static size_t next_char(size_t at)
{
	if (line[at])
		at++;
	while (((unsigned char)line[at] & 0xc0) == 0x80)
		at++;
	return at;
}

static void delete_range(size_t from, size_t to)
{
	memmove(line + from, line + to, strlen(line + to) + 1);
	cur = from;
	update_matches();
}

static void move_sel(int d)
{
	if (!nmatches)
		return;
	int was_sel = sel, was_top = top;
	sel = MAX(MIN(sel < 0 ? (d > 0 ? 0 : nmatches - 1) : sel + d, nmatches - 1), 0);
	show_sel();
	if (sel != was_sel || top != was_top)
		redraw();
}

/* A held key at the end of the line would repaint for nothing. */
static void move_cur(size_t to)
{
	if (to != cur)
		redraw();
	cur = to;
}

static void key(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	xkb_keysym_t ks = k->sym;
	const char *txt = k->utf8;
	int n = (int)strlen(txt);
	if (k->mods & (MTK_MOD_ALT | MTK_MOD_LOGO))
		return;
	if (k->mods & MTK_MOD_CTRL) {
		switch (k->latin) {
		case XKB_KEY_q: case XKB_KEY_Q: case XKB_KEY_w: case XKB_KEY_W:
			exit(0);
		case XKB_KEY_u: case XKB_KEY_U:
			line[0] = 0;
			cur = 0;
			update_matches();
			break;
		}
		return;
	}
	switch (ks) {
	case XKB_KEY_Escape:
		exit(0);
	case XKB_KEY_F6:        /* nothing, but the tests wait for it */
		syncs++;
		return;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		activate();
		return;
	case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab:
		take_chosen();
		return;
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
		move_sel(-1);
		return;
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
		move_sel(1);
		return;
	case XKB_KEY_Prior: case XKB_KEY_KP_Prior:
		move_sel(-rows_vis());
		return;
	case XKB_KEY_Next: case XKB_KEY_KP_Next:
		move_sel(rows_vis());
		return;
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
		move_cur(prev_char(cur));
		return;
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
		move_cur(next_char(cur));
		return;
	case XKB_KEY_Home: case XKB_KEY_KP_Home:
		move_cur(0);
		return;
	case XKB_KEY_End: case XKB_KEY_KP_End:
		move_cur(strlen(line));
		return;
	case XKB_KEY_BackSpace:
		if (cur)
			delete_range(prev_char(cur), cur);
		return;
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
		if (line[cur])
			delete_range(cur, next_char(cur));
		return;
	}
	if (n <= 0 || (unsigned char)txt[0] < 0x20 || txt[0] == 0x7f ||
	    strlen(line) + (size_t)n >= sizeof line)
		return;
	memmove(line + cur + n, line + cur, strlen(line + cur) + 1);
	memcpy(line + cur, txt, n);
	cur += n;
	update_matches();
}

/* A held key moves and types, but runs and takes nothing again. */
static int key_repeats(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	xkb_keysym_t ks = k->sym;
	if (k->mods & (MTK_MOD_ALT | MTK_MOD_LOGO | MTK_MOD_CTRL))
		return 0;
	switch (ks) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
	case XKB_KEY_Prior: case XKB_KEY_KP_Prior:
	case XKB_KEY_Next: case XKB_KEY_KP_Next:
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_BackSpace:
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
		return 1;
	}
	uint32_t c = xkb_keysym_to_utf32(ks);
	return c >= 0x20 && c != 0x7f;
}

/* The match under the pointer, or -1. */
static int row_at(int x, int y)
{
	if (x < r_list.x || x >= r_list.x + r_list.w || y < r_list.y + BEV + 2)
		return -1;
	int row = top + (y - r_list.y - BEV - 2) / rowh;
	if (row >= top + rows_vis() || row >= nmatches)
		return -1;
	return row;
}

static void ptr_button(uint32_t code, uint32_t time)
{
	if (code != BTN_LEFT)
		return;
	int row = row_at(ptr_x, ptr_y);
	if (row < 0)
		return;
	int twice = row == last_click_row && time - last_click_ms < DCLICK_MS;
	last_click_row = twice ? -1 : row;
	last_click_ms = time;
	sel = row;
	redraw();
	if (!twice)
		return;
	if (has_args()) {
		if (take_chosen())
			run_line();
	} else {
		run_prog(&progs[matches[sel]]);
	}
}

static void pointer(struct mtk_window *w, const struct mtk_pointer *p, void *data)
{
	ptr_x = (int)p->x;
	ptr_y = (int)p->y;
	if (p->kind == MTK_PRESS) {
		ptr_button(p->button, p->time);
	} else if (p->kind == MTK_SCROLL && p->steps_y) {
		/* Three rows a wheel step: the view moves, and what is chosen stays. */
		enum { ROWS_PER_STEP = 3 };
		int was_top = top;
		top = MAX(MIN(top + p->steps_y * ROWS_PER_STEP, nmatches - rows_vis()), 0);
		if (top != was_top)
			redraw();
	}
}

/* A new size shows what is chosen; focus alone keeps a scrolled view. */
static void configure(struct mtk_window *w, int cw, int ch, unsigned states, void *data)
{
	int was_w = W, was_h = H;
	mtk_window_size(w, &W, &H);
	layout();
	if (W != was_w || H != was_h)
		show_sel();
	else
		top = MAX(MIN(top, nmatches - rows_vis()), 0);
}

static void close_window(struct mtk_window *w, void *data)
{
	exit(0);
}

static const struct mtk_window_listener listener = {
	.paint = paint,
	.configure = configure,
	.close = close_window,
	.key = key,
	.key_repeats = key_repeats,
	.pointer = pointer,
};

/* ---- main ---------------------------------------------------------------- */

/* For the tests, as mwc's MWC_TEST_STATE: one "key value" per line, written
 * whole and renamed into place, only when something changed. */
static void state_write(void)
{
	static char last[2 * sizeof line + 512];
	char now[sizeof last], tmp[PATH_MAX];
	snprintf(now, sizeof now, "syncs %u\ndirs %u\ndraws %u\ndirty %d\ncursors %u\n"
		"sel %d\ntop %d\nmatches %d\nvisible %d\nline %s\nstatus %s\n", syncs,
		dirs_read, mtk_window_paints(win), mtk_window_dirty(win), mtk_cursor_loads(m),
		sel, top, nmatches, rows_vis(), line, status_msg);
	if (!strcmp(now, last) ||
	    (size_t)snprintf(tmp, sizeof tmp, "%s.tmp", test_state) >= sizeof tmp)
		return;
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(now, f);
	if (fclose(f) == 0 && rename(tmp, test_state) == 0)
		strcpy(last, now);
}

static void idle(struct mtk *mm, void *data)
{
	if (test_state)
		state_write();
}

int main(int argc, char **argv)
{
	/* With 0, 1 or 2 closed the Wayland socket could get one of them, and a
	 * child that can't exec would write its error into the connection. */
	int null;
	while ((null = open("/dev/null", O_RDWR)) >= 0 && null <= 2)
		;
	if (null > 2)
		close(null);
	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			printf("mrun %s\n", MRUN_VERSION);
			return 0;
		}
		int help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help");
		fprintf(stderr, "usage: mrun [-v]\n"
			"Environment: MRUN_FONT (default \"Sans 10\"), "
			"MWC_INACTIVE, MWC_ACTIVE.\n");
		return help ? 0 : 1;
	}
	setlocale(LC_ALL, "");
	test_state = getenv("MRUN_TEST_STATE");
	if (test_state && !*test_state)
		test_state = NULL;

	if (!(m = mtk_open(&(struct mtk_config){ .name = "mrun", .idle = idle })))
		return 1;
	theme_init();
	scan_path();
	update_matches();
	win = mtk_window_new(m, &(struct mtk_window_config){
		.width = W, .height = H, .min_width = 220,
		.min_height = 2 * PAD + 2 * barh + 8 + 3 * rowh + 2 * BEV + 4,
		.server_decoration = 1,
	}, &listener, NULL);
	layout();
	return mtk_run(m);
}
