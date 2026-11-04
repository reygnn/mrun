/*
 * mrun - a minimal run dialog for Wayland, in the Motif look of mwc
 * (https://github.com/reygnn/mwc) and mfm.
 *
 * libwayland, xkbcommon, Pango and Cairo, one file, no toolkit. It lists the
 * programs in $PATH, once at the start, and narrows them down while typing:
 * exact names first, then those starting with the text, then those containing it,
 * ignoring case. It reads the same colours as mwc: MWC_INACTIVE / MWC_ACTIVE from
 * the environment, else Mwm*background and Mwm*activeBackground from
 * ~/.config/mwc/mwmrc, else the Motif defaults.
 *
 * Keys:   typing  narrows the list      Up/Down/PgUp/PgDn  choose
 *         Return  run                   Tab  take the chosen name
 *         Left/Right/Home/End/Backspace/Delete  edit    Ctrl+U  clear
 *         Esc / Ctrl+Q / Ctrl+W  quit
 * Mouse:  click chooses, double click runs, wheel scrolls.
 *
 * Without a blank, Return runs the chosen program directly. With a blank, the line
 * goes to the shell as it is, so it can have arguments and shell syntax. Nothing
 * runs in a terminal, and what a program prints goes where mrun's output goes.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define DCLICK_MS 400
#ifndef MAX            /* glib has them too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct rect { int x, y, w, h; };

/* ---- state -------------------------------------------------------------- */

static char **names;            /* of the programs, sorted, no duplicates */
static int nnames;
static int *matches;            /* indexes into names, best first */
static int nmatches;
static int sel = -1, top;       /* chosen and first shown of the matches */
static char line[4096];
static size_t cur;              /* byte offset of the text cursor */
static char status_msg[256];
static uint32_t last_click_ms;
static int last_click_row = -1;
static int dirty = 1;

/* ---- Wayland ------------------------------------------------------------ */

/* One wl_shm buffer with a Cairo context on it. */
struct shmbuf {
	struct wl_buffer *wl;
	cairo_surface_t *cs;
	cairo_t *cr;
	void *data;
	size_t size;
	int w, h;
	int busy;       /* the compositor still reads it */
	int stale;      /* wrong size: free it as soon as it is released */
};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct wl_surface *surface;
static struct wl_callback *frame;       /* a frame is on its way */
static struct shmbuf *bufs[2];
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *toplevel;
static int configured;          /* the first configure has arrived */
static int need_commit;         /* a configure waits for its commit */
static int pend_w, pend_h;      /* size of the configure in progress */
static int pend_bound;          /* ... which is maximized, tiled or the like */
static int float_w = 420, float_h = 320; /* the size of its own to go back to */

static struct wl_pointer *pointer;
static struct wl_surface *ptr_surface;
static int ptr_x, ptr_y;
static double axis_acc;
static int axis_steps;
static struct wl_surface *cursor_surf;
static struct wl_cursor_image *cursor_img;

static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *kb_map;
static struct xkb_state *kb_state;
static struct xkb_compose_state *compose;
static uint32_t repeat_kc;      /* xkb keycode of the repeating key, or 0 */
static uint64_t repeat_next;
static int repeat_rate = 25, repeat_delay = 600;

static int W = 420, H = 320;
static uint32_t px_bg, px_top, px_bot, px_list, px_paper, px_sel;
static uint32_t xc_fg, xc_sel, xc_dim, xc_err;
static cairo_t *cr;             /* what fill, bevel and text draw on */
static PangoLayout *plo;
static int font_asc, font_desc;
static int rowh, barh;
static struct rect r_input, r_list, r_status;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the programs in $PATH ---------------------------------------------- */

static int name_cmp(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
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
				char **grown = realloc(names, cap * sizeof *names);
				if (!grown) {
					fprintf(stderr, "mrun: out of memory\n");
					exit(1);
				}
				names = grown;
			}
			if (!(names[nnames] = strdup(e->d_name))) {
				fprintf(stderr, "mrun: out of memory\n");
				exit(1);
			}
			nnames++;
		}
		closedir(d);
	}
	free(copy);
	qsort(names, nnames, sizeof *names, name_cmp);
	/* The same name twice: the shell would only ever find one of them. */
	int kept = 0;
	for (int i = 0; i < nnames; i++) {
		if (kept && !strcmp(names[kept - 1], names[i]))
			free(names[i]);
		else
			names[kept++] = names[i];
	}
	nnames = kept;
	if (!(matches = malloc((size_t)(nnames + 1) * sizeof *matches))) {
		fprintf(stderr, "mrun: out of memory\n");
		exit(1);
	}
}

/* The length of the first word of the line. */
static size_t first_word(void)
{
	return strcspn(line, " \t");
}

static int has_args(void)
{
	return line[first_word()] != 0;
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
	size_t qlen = first_word();
	char q[sizeof line];
	memcpy(q, line, qlen);
	q[qlen] = 0;
	nmatches = 0;
	for (int rank = 0; rank < 3; rank++) {
		for (int i = 0; i < nnames; i++) {
			const char *n = names[i];
			int is_prefix = !strncasecmp(n, q, qlen);
			int is_exact = is_prefix && !n[qlen];
			if (rank == 0 ? is_exact :
			    rank == 1 ? is_prefix && !is_exact :
			    !is_prefix && strcasestr(n, q))
				matches[nmatches++] = i;
		}
	}
	/* With nothing typed nothing is chosen, so Return doesn't run the first of all. */
	sel = qlen && nmatches ? 0 : -1;
	top = 0;
	status_msg[0] = 0;
	dirty = 1;
}

/* ---- running ------------------------------------------------------------ */

/* Starts it in a session of its own and ends here, unless it can't be started. */
static void run_argv(const char *file, char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0) {
		snprintf(status_msg, sizeof status_msg, "cannot start: %s",
			strerror(errno));
		dirty = 1;
		return;
	}
	if (pid == 0) {
		setsid();
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0) {
			dup2(null, 0);
			if (null != 0)
				close(null);
		}
		execvp(file, argv);
		fprintf(stderr, "mrun: %s: %s\n", file, strerror(errno));
		_exit(127);
	}
	exit(0);
}

static void run_name(const char *name)
{
	char *const argv[] = { (char *)name, NULL };
	run_argv(name, argv);
}

static void run_line(void)
{
	char *const argv[] = { (char *)"sh", (char *)"-c", line, NULL };
	run_argv("/bin/sh", argv);
}

/* Return. With a blank the line is what the user wants, whatever is chosen. */
static void activate(void)
{
	if (!line[0] && sel < 0)
		return;
	if (has_args()) {
		run_line();
	} else if (sel >= 0) {
		run_name(names[matches[sel]]);
	} else if (strchr(line, '/')) {
		run_line();
	} else {
		snprintf(status_msg, sizeof status_msg, "not in $PATH: %.200s", line);
		dirty = 1;
	}
}

/* Puts the chosen name in place of the first word; what follows stays. */
static void take_chosen(void)
{
	if (sel < 0)
		return;
	const char *n = names[matches[sel]];
	size_t old = first_word(), nl = strlen(n), rest = strlen(line) - old;
	int blank = !rest;
	if (nl + rest + 2 > sizeof line)
		return;
	memmove(line + nl, line + old, rest + 1);
	memcpy(line, n, nl);
	if (blank) {
		line[nl] = ' ';
		line[nl + 1] = 0;
	}
	cur = nl + 1;
	update_matches();
}

/* ---- buffers ------------------------------------------------------------ */

static void buf_free(struct shmbuf *b)
{
	cairo_destroy(b->cr);
	cairo_surface_destroy(b->cs);
	wl_buffer_destroy(b->wl);
	munmap(b->data, b->size);
	free(b);
}

static void buf_release(void *data, struct wl_buffer *wl)
{
	struct shmbuf *b = data;
	b->busy = 0;
	if (b->stale)
		buf_free(b);
}

static const struct wl_buffer_listener buf_listener = { buf_release };

static struct shmbuf *buf_new(int w, int h)
{
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
	size_t size = (size_t)stride * h;
	struct shmbuf *b = calloc(1, sizeof *b);
	int fd = memfd_create("mrun", MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mrun: no memory for a %dx%d buffer: %s\n", w, h,
			strerror(errno));
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	b->data = data;
	b->size = size;
	b->w = w;
	b->h = h;
	b->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, w, h,
		stride);
	b->cr = cairo_create(b->cs);
	cairo_set_antialias(b->cr, CAIRO_ANTIALIAS_NONE);       /* not the text */
	return b;
}

/* A buffer of this size to draw on, or NULL while the compositor holds both;
 * its release wakes the main loop. */
static struct shmbuf *get_buffer(int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct shmbuf *b = bufs[i];
		if (!b || (b->w == w && b->h == h))
			continue;
		if (b->busy)
			b->stale = 1;
		else
			buf_free(b);
		bufs[i] = NULL;
	}
	for (int i = 0; i < 2; i++)
		if (bufs[i] && !bufs[i]->busy)
			return bufs[i];
	for (int i = 0; i < 2; i++)
		if (!bufs[i])
			return bufs[i] = buf_new(w, h);
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	wl_callback_destroy(cb);
	frame = NULL;
}

static const struct wl_callback_listener frame_listener = { frame_done };

/* ---- colours (same rules as mwm's draw.c) ------------------------------- */

static uint32_t mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

static uint32_t contrast(uint32_t bg)
{
	unsigned r = (bg >> 16) & 0xff, g = (bg >> 8) & 0xff, b = bg & 0xff;
	return 299 * r + 587 * g + 114 * b > 140000 ? 0x000000 : 0xffffff;
}

/* What mwc reads: "#rrggbb", names like "gold", and rgb:r/g/b with 1 to 4 hex
 * digits per channel. */
static int parse_color(const char *s, uint32_t *out)
{
	PangoColor c;
	if (!s || !*s)
		return 0;
	if (!strncasecmp(s, "rgb:", 4)) {
		uint32_t rgb = 0;
		s += 4;
		for (int i = 0; i < 3; i++) {
			char *end;
			unsigned long v = strtoul(s, &end, 16);
			int digits = end - s;
			if (digits < 1 || digits > 4 || *end != (i < 2 ? '/' : '\0'))
				return 0;
			unsigned long max = (1ul << (4 * digits)) - 1;
			rgb = rgb << 8 | (v * 255 + max / 2) / max;
			s = end + 1;
		}
		*out = rgb;
		return 1;
	}
	if (!pango_color_parse(&c, s))
		return 0;
	*out = (uint32_t)(c.red >> 8) << 16 | (c.green >> 8) << 8 | (c.blue >> 8);
	return 1;
}

/* Last global "Mwm*key: value" line of ~/.config/mwc/mwmrc. */
static char *mwmrc_get(const char *key, char *val, size_t n)
{
	char path[PATH_MAX], text_line[512];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc",
			home ? home : "");
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL;
	size_t kl = strlen(key);
	while (fgets(text_line, sizeof text_line, f)) {
		char *p = text_line;
		while (isspace((unsigned char)*p))
			p++;
		/* As mwc reads it: any case, "." or "*", blanks before the ":". */
		char *colon = strchr(p, ':');
		if (!colon)
			continue;
		char *ke = colon;
		while (ke > p && isspace((unsigned char)ke[-1]))
			ke--;
		if (strncasecmp(p, "mwm", 3) || (p[3] != '*' && p[3] != '.') ||
		    (size_t)(ke - (p + 4)) != kl || strncasecmp(p + 4, key, kl))
			continue;
		p = colon + 1;
		while (isspace((unsigned char)*p))
			p++;
		char *e = p + strlen(p);
		while (e > p && isspace((unsigned char)e[-1]))
			*--e = 0;
		if ((size_t)snprintf(val, n, "%s", p) < n)
			found = val;
	}
	fclose(f);
	return found;
}

static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	char v[256];
	if (parse_color(getenv(env), &rgb))
		return rgb;
	if (parse_color(mwmrc_get(res, v, sizeof v), &rgb))
		return rgb;
	return def;
}

static void theme_init(void)
{
	uint32_t bg = setting("MWC_INACTIVE", "background", 0xaeb2c3);
	uint32_t fg = contrast(bg);

	px_bg = bg;
	px_top = mix(bg, 0xffffff, 0.55f);
	px_bot = mix(bg, 0x000000, 0.5f);
	px_list = mix(bg, 0xffffff, 0.3f);
	px_paper = 0xf6f6f6;
	px_sel = setting("MWC_ACTIVE", "activeBackground", 0x7387ad);
	xc_fg = contrast(px_list);
	xc_sel = contrast(px_sel);
	xc_dim = mix(bg, fg, 0.6f);
	xc_err = fg == 0xffffff ? 0xffa0a0 : 0x8a0303; /* readable on dark themes */

	const char *f = getenv("MRUN_FONT");
	PangoFontDescription *desc =
		pango_font_description_from_string(f && *f ? f : "Sans 10");
	/* "Monospace" has no size, and metrics without one are all zero. */
	if (pango_font_description_get_size(desc) <= 0)
		pango_font_description_set_size(desc, 10 * PANGO_SCALE);
	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	plo = pango_layout_new(pctx);
	pango_layout_set_font_description(plo, desc);
	pango_layout_set_single_paragraph_mode(plo, TRUE);
	PangoFontMetrics *m = pango_context_get_metrics(pctx, desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(m));
	pango_font_metrics_unref(m);
	pango_font_description_free(desc);
	rowh = font_asc + font_desc + 4;
	barh = rowh + 6;
}

/* ---- drawing ------------------------------------------------------------ */

static void set_rgb(uint32_t c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void fill(uint32_t px, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	set_rgb(px);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void bevel(struct rect r, int raised)
{
	uint32_t t = raised ? px_top : px_bot;
	uint32_t b = raised ? px_bot : px_top;
	for (int i = 0; i < BEV; i++) {
		fill(t, r.x + i, r.y + i, r.w - 2 * i, 1);
		fill(t, r.x + i, r.y + i, 1, r.h - 2 * i);
		fill(b, r.x + i + 1, r.y + r.h - 1 - i, r.w - 2 * i - 1, 1);
		fill(b, r.x + r.w - 1 - i, r.y + i + 1, 1, r.h - 2 * i - 1);
	}
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

static void paint(void)
{
	char s[sizeof status_msg];
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
		if (i == sel)
			fill(px_sel, lx + 1, y, lw - 2, rowh);
		text(i == sel ? xc_sel : xc_fg, lx + 5, y, rowh, lw - 10,
			names[matches[i]]);
	}
	cairo_restore(cr);

	bevel(r_status, 0);
	int sx = r_status.x + BEV + 4, sw = r_status.w - 2 * BEV - 8;
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
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(void)
{
	struct shmbuf *b = get_buffer(W, H);
	if (!b)
		return 0;
	cr = b->cr;
	paint();
	cairo_surface_flush(b->cs);
	if (frame)
		wl_callback_destroy(frame);
	frame = wl_surface_frame(surface);
	wl_callback_add_listener(frame, &frame_listener, NULL);
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, b->w, b->h);
	wl_surface_commit(surface);
	b->busy = 1;
	return 1;
}

/* ---- input -------------------------------------------------------------- */

static int mod_on(const char *name)
{
	return xkb_state_mod_name_is_active(kb_state, name,
		XKB_STATE_MODS_EFFECTIVE) > 0;
}

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
	sel = MAX(MIN(sel < 0 ? (d > 0 ? 0 : nmatches - 1) : sel + d, nmatches - 1), 0);
	show_sel();
	dirty = 1;
}

static void key(xkb_keysym_t ks, const char *txt, int n)
{
	if (mod_on(XKB_MOD_NAME_ALT) || mod_on(XKB_MOD_NAME_LOGO))
		return;
	if (mod_on(XKB_MOD_NAME_CTRL)) {
		switch (ks) {
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
		cur = prev_char(cur);
		dirty = 1;
		return;
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
		cur = next_char(cur);
		dirty = 1;
		return;
	case XKB_KEY_Home: case XKB_KEY_KP_Home:
		cur = 0;
		dirty = 1;
		return;
	case XKB_KEY_End: case XKB_KEY_KP_End:
		cur = strlen(line);
		dirty = 1;
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
static int key_repeats(uint32_t kc)
{
	xkb_keysym_t ks = xkb_state_key_get_one_sym(kb_state, kc);
	if (mod_on(XKB_MOD_NAME_ALT) || mod_on(XKB_MOD_NAME_LOGO) ||
	    mod_on(XKB_MOD_NAME_CTRL))
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

/* One key press or repeat, with kc an xkb keycode. Compose and dead keys come
 * from xkbcommon; a key that is only part of a sequence does nothing else. */
static void key_press(uint32_t kc)
{
	char txt[64];
	int n = -1;
	xkb_keysym_t ks = xkb_state_key_get_one_sym(kb_state, kc);
	if (compose && xkb_compose_state_feed(compose, ks) ==
	    XKB_COMPOSE_FEED_ACCEPTED) {
		switch (xkb_compose_state_get_status(compose)) {
		case XKB_COMPOSE_COMPOSING:
			return;
		case XKB_COMPOSE_CANCELLED:
			xkb_compose_state_reset(compose);
			return;
		case XKB_COMPOSE_COMPOSED:
			n = xkb_compose_state_get_utf8(compose, txt, sizeof txt);
			ks = xkb_compose_state_get_one_sym(compose);
			xkb_compose_state_reset(compose);
			break;
		case XKB_COMPOSE_NOTHING:
			break;
		}
	}
	if (n < 0)
		n = xkb_state_key_get_utf8(kb_state, kc, txt, sizeof txt);
	if (n >= (int)sizeof txt)
		n = 0;
	key(ks, txt, n);
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format,
	int32_t fd, uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (s == MAP_FAILED)
		return;
	struct xkb_keymap *map = xkb_keymap_new_from_string(xkb, s,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	struct xkb_state *state = map ? xkb_state_new(map) : NULL;
	if (!state) {
		xkb_keymap_unref(map);
		return;
	}
	xkb_state_unref(kb_state);
	xkb_keymap_unref(kb_map);
	kb_state = state;
	kb_map = map;
	repeat_kc = 0;
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
	repeat_kc = 0;
	if (compose)
		xkb_compose_state_reset(compose);
}

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	uint32_t kc = code + 8;
	if (!kb_state)
		return;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (kc == repeat_kc)
			repeat_kc = 0;
		return;
	}
	/* A modifier does not stop a key that is repeating. */
	int repeats = xkb_keymap_key_repeats(kb_map, kc);
	if (repeats)
		repeat_kc = 0;
	key_press(kc);
	if (repeats && repeat_rate > 0 && key_repeats(kc)) {
		repeat_kc = kc;
		repeat_next = now_ms() + repeat_delay;
	}
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (kb_state)
		xkb_state_update_mask(kb_state, depressed, latched, locked, 0, 0,
			group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *k, int32_t rate,
	int32_t delay)
{
	repeat_rate = rate;
	repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	ptr_surface = s;
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	if (cursor_img) {
		wl_pointer_set_cursor(p, serial, cursor_surf, cursor_img->hotspot_x,
			cursor_img->hotspot_y);
		wl_surface_attach(cursor_surf,
			wl_cursor_image_get_buffer(cursor_img), 0, 0);
		wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width,
			cursor_img->height);
		wl_surface_commit(cursor_surf);
	}
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	ptr_surface = NULL;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
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

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	if (code != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED ||
	    !ptr_surface)
		return;
	int row = row_at(ptr_x, ptr_y);
	if (row < 0)
		return;
	int twice = row == last_click_row && time - last_click_ms < DCLICK_MS;
	last_click_row = twice ? -1 : row;
	last_click_ms = time;
	sel = row;
	dirty = 1;
	if (!twice)
		return;
	if (has_args()) {
		take_chosen();
		run_line();
	} else {
		run_name(names[matches[sel]]);
	}
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc += wl_fixed_to_double(value);
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc = 0;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
	int32_t steps)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_steps += steps;
}

/* Wheels report steps; touchpads only a distance, of which AXIS_STEP makes
 * one wheel step. The view moves, and what is chosen stays. */
static void ptr_frame(void *data, struct wl_pointer *p)
{
	enum { AXIS_STEP = 15, ROWS_PER_STEP = 3 };
	int steps = axis_steps;
	if (steps) {
		axis_acc = 0;
	} else {
		steps = (int)(axis_acc / AXIS_STEP);
		axis_acc -= steps * AXIS_STEP;
	}
	axis_steps = 0;
	if (!steps)
		return;
	top = MAX(MIN(top + steps * ROWS_PER_STEP, nmatches - rows_vis()), 0);
	dirty = 1;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	int want_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	int want_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (want_ptr && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!want_ptr && pointer) {
		wl_pointer_release(pointer);
		pointer = NULL;
		ptr_surface = NULL;
	}
	if (want_kb && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!want_kb && keyboard) {
		wl_keyboard_release(keyboard);
		keyboard = NULL;
		repeat_kc = 0;
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- window setup ------------------------------------------------------- */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 4));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
			MIN(ver, 2));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		deco_mgr = wl_registry_bind(r, name,
			&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void main_xdg_configure(void *data, struct xdg_surface *s,
	uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	W = pend_w > 0 ? pend_w : float_w;
	H = pend_h > 0 ? pend_h : float_h;
	if (!pend_bound) {
		float_w = W;
		float_h = H;
	}
	layout();
	show_sel();
	configured = 1;
	need_commit = 1;
	dirty = 1;
}

static const struct xdg_surface_listener main_xdg_listener = {
	main_xdg_configure,
};

/* 0 leaves the size to us, which after maximizing is the size from before. */
static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
	uint32_t *st;
	pend_w = w;
	pend_h = h;
	pend_bound = 0;
	wl_array_for_each(st, states)
		if (*st != XDG_TOPLEVEL_STATE_ACTIVATED &&
		    *st != XDG_TOPLEVEL_STATE_RESIZING)
			pend_bound = 1;
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	exit(0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

static void cursor_init(void)
{
	const char *s = getenv("XCURSOR_SIZE");
	int size = s && atoi(s) > 0 ? atoi(s) : 24;
	struct wl_cursor_theme *theme =
		wl_cursor_theme_load(getenv("XCURSOR_THEME"), size, shm);
	if (!theme)
		return;
	struct wl_cursor *c = wl_cursor_theme_get_cursor(theme, "default");
	if (!c)
		c = wl_cursor_theme_get_cursor(theme, "left_ptr");
	if (!c)
		return;
	cursor_img = c->images[0];
	cursor_surf = wl_compositor_create_surface(compositor);
}

/* The locale compose sequences follow, in the order of xkbcommon's docs. */
static void compose_init(void)
{
	const char *loc = getenv("LC_ALL");
	if (!loc || !*loc)
		loc = getenv("LC_CTYPE");
	if (!loc || !*loc)
		loc = getenv("LANG");
	if (!loc || !*loc)
		loc = "C";
	struct xkb_compose_table *table = xkb_compose_table_new_from_locale(xkb,
		loc, XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!table)
		return;
	compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
	xkb_compose_table_unref(table);
}

static void setup_window(void)
{
	surface = wl_compositor_create_surface(compositor);
	main_xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(main_xdg, &main_xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(main_xdg);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "mrun");
	xdg_toplevel_set_title(toplevel, "mrun");
	xdg_toplevel_set_min_size(toplevel, 220,
		2 * PAD + 2 * barh + 8 + 3 * rowh + 2 * BEV + 4);
	if (deco_mgr) {
		struct zxdg_toplevel_decoration_v1 *d =
			zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr,
				toplevel);
		zxdg_toplevel_decoration_v1_set_mode(d,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	layout();
}

static void lost_connection(void)
{
	fprintf(stderr, "mrun: lost the connection to the compositor: %s\n",
		strerror(wl_display_get_error(dpy)));
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		int help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help");
		fprintf(stderr, "usage: mrun\n"
			"Environment: MRUN_FONT (default \"Sans 10\"), "
			"MWC_INACTIVE, MWC_ACTIVE.\n");
		return help ? 0 : 1;
	}
	setlocale(LC_ALL, "");

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "mrun: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!(xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		fprintf(stderr, "mrun: cannot set up xkbcommon\n");
		return 1;
	}
	compose_init();
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "mrun: the compositor lacks wl_compositor, wl_shm "
			"or xdg_wm_base\n");
		return 1;
	}
	cursor_init();
	theme_init();
	scan_path();
	update_matches();
	setup_window();
	wl_surface_commit(surface);

	int dfd = wl_display_get_fd(dpy);
	for (;;) {
		if (wl_display_dispatch_pending(dpy) < 0)
			lost_connection();
		uint64_t now = now_ms();
		if (repeat_kc && now >= repeat_next) {
			/* The key may mean something else by now. */
			if (key_repeats(repeat_kc))
				key_press(repeat_kc);
			else
				repeat_kc = 0;
			repeat_next = now_ms() + MAX(1000 / MAX(repeat_rate, 1), 1);
		}
		/* A frame at a time, except that a configure wants its commit
		 * even if the window is hidden and gets no frames. */
		if (dirty && configured && (!frame || need_commit) && draw()) {
			dirty = 0;
			need_commit = 0;
		}

		if (wl_display_prepare_read(dpy) != 0)
			continue;       /* events arrived in the meantime */
		short dev = POLLIN;
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			dev |= POLLOUT;
		}
		int timeout = -1;
		if (repeat_kc) {
			now = now_ms();
			timeout = repeat_next > now ? (int)MIN(repeat_next - now,
				(uint64_t)INT_MAX) : 0;
		}
		struct pollfd pfd = { .fd = dfd, .events = dev };
		poll(&pfd, 1, timeout);
		if (pfd.revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
	}
}
