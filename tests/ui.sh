#!/bin/bash
# Black-box test of mrun under a headless mwc with simulated mouse and keyboard. A PATH
# of small programs here shows what was started, and with what. It has its own Wayland
# socket and starts nothing of the system.
#
# Needs a built mwc with build/vinput and build/pngtool in MWC_DIR (by default ../mwc),
# and grim. Output goes to build/ui.log, mrun's own to build/ui-mrun.log.
MRUN_DIR=$(cd "$(dirname "$0")/.." && pwd)
MRUN=${MRUN:-$MRUN_DIR/mrun}
LOG=$MRUN_DIR/build/ui.log
MRUN_LOG=$MRUN_DIR/build/ui-mrun.log
MWC_DIR=${MWC_DIR:-$MRUN_DIR/../mwc}
[ -x "$MRUN" ] || { echo "FAIL: $MRUN not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim
for tool in build/vinput build/pngtool; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
export HOME=$SMOKE_DIR/home
unset MWC_INACTIVE MWC_ACTIVE MRUN_FONT
mkdir -p "$HOME"
: >"$MRUN_LOG"
SEL=7387ad
TOP=dbdce4      # the default background's light shade, top and left of a bevel
MARKS=$SMOKE_DIR/marks
A=$SMOKE_DIR/bin-a
Z=$SMOKE_DIR/bin-z
mkdir -p "$MARKS" "$A" "$Z"

# A program that writes its arguments to the mark of that name.
program() { # <dir> <name> [<mark>]
	printf '#!/bin/sh\necho "$*" >"%s/%s"\n' "$MARKS" "${3:-$2}" >"$1/$2"
	chmod +x "$1/$2"
}

program "$A" alpha
program "$A" alphabet
program "$A" beta
program "$A" mybeta
program "$A" hidden
mv "$A/hidden" "$A/.hidden"
program "$Z" beta otherbeta     # found second in PATH: never to run
printf '#!/bin/sh\necho x >"%s/delta"\n' "$MARKS" >"$A/delta"     # not executable
mkdir "$A/epsilon"
ln -s alpha "$A/zeta"           # a link counts
printf '#!/bin/sh\necho $$ >"%s/omega"\nexec /bin/sleep 60\n' "$MARKS" >"$A/omega"
chmod +x "$A/omega"
program "$A" gone
program "$A" Gamma
program "$A" gamma lowgamma
program "$Z" Gamma othergamma   # the same name again, later in PATH
program "$A" "two words" twowords
program "$A" "it's" its
program "$A" badok
program "$A" "$(printf 'bad\001')" badctl    # a control character: not offered
program "$A" "$(printf 'bad\377')" badutf    # not UTF-8: not offered
for i in $(seq -w 1 30); do program "$A" "zz$i"; done   # more than fit
PROGS=$A:$Z
# Names the shell could misread, kept out of the list the other tests count on.
Q=$SMOKE_DIR/bin-q
mkdir -p "$Q"
program "$Q" -dash dash
program "$Q" x=y xy
program "$Q" if ifword
program "$Q" été ete
ln -s bin-a "$SMOKE_DIR/alink"

# Compose sequences of plain keys: é, ten of them, and the longest runs of x that
# fit in a key's text (63 bytes), and 54 of them.
X63=$(printf '%063d' 0 | tr 0 x)
X54=${X63:9}
{
	printf '<q> <a> : "alpha"\n'
	printf '<q> <e> : "é"\n'
	printf '<q> <E> : "É"\n'
	printf '<q> <u> : "éééééééééé"\n'
	printf '<q> <x> : "%s"\n' "$X63"
	printf '<q> <b> : "%s"\n' "$X54"
} >"$SMOKE_DIR/Compose"

MSTATE=$SMOKE_DIR/mrun-state
mrun_start() { # [PATH [directory [nostderr]]]
	rm -f "$MSTATE"
	(cd "${2:-.}" || exit
		[ "${3:-}" = nostderr ] && exec 2>&-
		[ "${3:-}" = brokenstderr ] && exec 2> >(exec true)
		PATH=${1:-$PROGS} WAYLAND_DISPLAY=wayland-0 \
		MRUN_TEST_STATE=$MSTATE exec "$MRUN") >>"$MRUN_LOG" 2>&1 &
	MRUN_PID=$!
	check "mrun has a focused window" '[ "$(get mrun kbfocus)" = 1 ]'
	CX=$(($(get mrun x) + B))
	CY=$(($(get mrun y) + T + B))
}

type_text() {
	local i c
	for ((i = 0; i < ${#1}; i++)); do
		c=${1:i:1}
		case $c in
		" ") c=space ;;
		/) c=slash ;;
		";") c=semicolon ;;
		.) c=period ;;
		-) c=minus ;;
		"'") c=apostrophe ;;
		[A-Z]) c=shift+${c,} ;;
		esac
		do_input key "$c"
	done
}

# mstate <key>: from the state mrun writes for the tests.
mstate() {
	sed -n "s/^$1 //p" "$MSTATE" 2>/dev/null
}

# Until mrun has handled all input so far: it counts F6 and does nothing else
# with it, and keys come in order.
mrun_sync() {
	local n i
	n=$(mstate syncs)
	do_input key F6
	for ((i = 0; i < 100; i++)); do
		kill -0 "$MRUN_PID" 2>/dev/null || return 1
		[ "$(mstate syncs)" -gt "${n:-0}" ] 2>/dev/null && return 0
		sleep 0.05
	done
	fail "mrun didn't handle F6"
}

# mrun_draws: how many frames mrun has drawn once all input so far is drawn.
mrun_draws() {
	local i
	mrun_sync || fail "mrun ended"
	for ((i = 0; i < 100; i++)); do
		[ "$(mstate dirty)" = 0 ] && { mstate draws; return 0; }
		sleep 0.05
	done
	fail "mrun has still not drawn"
}

# expect_state <description> <key> <value>
expect_state() {
	mrun_sync || fail "$1: mrun ended"
	[ "$(mstate "$2")" = "$3" ] || fail "$1: $2 is '$(mstate "$2")', not '$3'"
	echo "ok: $1"
}

mrun_exits() { # <description>: must end by itself, and with status 0
	local i status
	for ((i = 0; i < 50; i++)); do
		kill -0 "$MRUN_PID" 2>/dev/null || break
		sleep 0.1
	done
	kill -0 "$MRUN_PID" 2>/dev/null && fail "$1: mrun is still running"
	wait "$MRUN_PID"
	status=$?
	[ "$status" = 0 ] || fail "$1: mrun exited with status $status"
	check "$1: the window is gone" '[ -z "$(get mrun w)" ]'
}

expect_ran() { # <description> <mark> <arguments>
	local i
	mrun_exits "$1"
	# Not empty: echo creates the file before it writes, and always writes a newline.
	for ((i = 0; i < 50; i++)); do
		[ -s "$MARKS/$2" ] && break
		sleep 0.1
	done
	[ -s "$MARKS/$2" ] || fail "$1: $2 didn't run (ran: $(ls "$MARKS" | xargs))"
	[ "$(cat "$MARKS/$2")" = "$3" ] || fail "$1: $2 got '$(cat "$MARKS/$2")', not '$3'"
	rm "$MARKS/$2"
	[ -z "$(ls "$MARKS")" ] || fail "$1: also ran $(ls "$MARKS" | xargs)"
	echo "ok: $1"
}

still_open() { # <description>: nothing ran, and it is there
	mrun_sync || fail "$1: mrun ended"
	[ -z "$(ls "$MARKS")" ] || fail "$1: ran $(ls "$MARKS" | xargs)"
	echo "ok: $1"
}

# mrun_focus: from the refreshed mwc state, how many mrun windows there are and
# where the one with the keyboard is, as "N X,Y".
mrun_focus() {
	awk '$1 == "view" && $2 == "app=mrun" {
		n++
		for (i = 3; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
		if (v["kbfocus"] == 1) at = v["x"] "," v["y"]
	} END { print n + 0, at }' "$MWC_TEST_STATE"
}

# second_mrun: another mrun takes the keyboard; second_mrun_end gives it back.
second_mrun() {
	refresh
	FIRST_AT=$(get mrun x),$(get mrun y)
	(PATH=$PROGS WAYLAND_DISPLAY=wayland-0 exec "$MRUN") >>"$MRUN_LOG" 2>&1 &
	MRUN2_PID=$!
	check "a second mrun has the keyboard" \
		'set -- $(mrun_focus); [ "$1" = 2 ] && [ -n "$2" ] && [ "$2" != "$FIRST_AT" ]'
}
second_mrun_end() {
	kill "$MRUN2_PID"
	wait "$MRUN2_PID"
	check "the first has it back" '[ "$(mrun_focus)" = "1 $FIRST_AT" ]'
}

mrun_quit() {
	do_input key Escape
	mrun_exits "Esc"
}

trap 'WAYLAND_DISPLAY=wayland-0 grim "$MRUN_DIR/build/ui-last.png" 2>/dev/null; cleanup' EXIT

start_mwc 'touch "$SMOKE_DIR/up"; exec sleep 600'
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput 1280 720 us,ru     # a second layout without Latin letters
refresh
B=$(global theme border)
T=$(global theme title)

# ---- finding programs ----------------------------------------------------------

mrun_start
check "mwc gave it a frame" '[ "$(get mrun frame)" = 1 ]'
type_text alpha
do_input key Return
expect_ran "the exact name" alpha ""

mrun_start
type_text alp
do_input key Return
expect_ran "the start of a name takes the first" alpha ""

mrun_start
type_text alp
do_input key Down
do_input key Return
expect_ran "Down chooses the next" alphabet ""

mrun_start
type_text alp
do_input key Down
do_input key Down
do_input key Down
do_input key Up
do_input key Return
expect_ran "Up goes back, and the end of the list stops it" alpha ""

mrun_start
type_text alp
do_input key Down
do_input key Down
do_input key Return
expect_ran "Down stops at the last, it doesn't go round" alphabet ""

mrun_start
do_input key shift+a
type_text lp
do_input key Return
expect_ran "case doesn't matter" alpha ""

# beta starts with it, alphabet and mybeta only contain it, in that order.
mrun_start
type_text bet
do_input key Return
expect_ran "a name starting with it goes first" beta ""
mrun_start
type_text bet
do_input key Down
do_input key Return
expect_ran "then names containing it" alphabet ""

mrun_start
type_text my
do_input key Return
expect_ran "the middle or end of a name is found" mybeta ""

mrun_start
type_text zeta
do_input key Return
expect_ran "a link counts" alpha ""

mrun_start "$A:$A/:$SMOKE_DIR/alink:$Z"
expect_state "a directory named again in PATH is read once" dirs 2
mrun_quit

# ---- what isn't a program -------------------------------------------------------

for name in nothing delta epsilon hidden; do
	mrun_start
	type_text "$name"
	do_input key Return
	still_open "$name isn't offered"
	mrun_quit
done

mrun_start
type_text bad
expect_state "names with a control character or not UTF-8 aren't offered" matches 1
mrun_quit

mrun_start
type_text nosuch
do_input key Return
expect_state "the status line says what isn't there" status 'not in $PATH: nosuch'
mrun_quit

mrun_start
do_input key Return
still_open "Return on an empty line does nothing"
do_input key Down
do_input key Return
expect_ran "but Down chooses the first of all" alpha ""

mrun_start
do_input key Up
do_input key Return
expect_ran "and Up the last" zz30 ""

mrun_start
type_text "  "
do_input key Return
still_open "Return on a line of blanks does nothing"
do_input key Down
do_input key Return
expect_ran "but runs what is chosen" alpha ""

# A relative PATH entry means the directory it happens to be started in.
mkdir "$SMOKE_DIR/cwd"
program "$SMOKE_DIR/cwd" rel
program "$SMOKE_DIR/cwd" alpha dotalpha
mrun_start ".:$A" "$SMOKE_DIR/cwd"
type_text rel
do_input key Return
still_open "a relative PATH entry doesn't count"
mrun_quit
# Nor when starting: alpha is the one in the list.
mrun_start ".:$A" "$SMOKE_DIR/cwd"
type_text alpha
do_input key Return
expect_ran "a name runs from where it was found, not from ./" alpha ""

mrun_start "$SMOKE_DIR/nonexistent"
type_text alpha
do_input key Return
still_open "no programs at all"
mrun_quit

# ---- arguments -----------------------------------------------------------------

mrun_start
type_text "beta one two"
do_input key Return
expect_ran "a line with a blank gets its arguments" beta "one two"

mrun_start
type_text "alp"
do_input key Tab
type_text x
do_input key Return
expect_ran "Tab takes the name and leaves room for arguments" alpha "x"

mrun_start
type_text "alp x"
do_input key Down
do_input key Tab
do_input key Return
expect_ran "Tab replaces the first word and keeps the rest" alphabet "x"

mrun_start
do_input key Tab
still_open "Tab with nothing chosen does nothing"
mrun_quit

mrun_start
type_text two
do_input key Tab
type_text x
do_input key Return
expect_ran "Tab quotes a name with a blank" twowords "x"

mrun_start
type_text "it'"
do_input key Tab
type_text x
do_input key Return
expect_ran "and one with a quote" its "x"

# The list after Tab is that name, still chosen, and Tab again changes nothing.
for name in two "it'"; do
	mrun_start
	type_text "$name"
	do_input key Tab
	expect_state "Tab on '$name' keeps it chosen" sel 0
	expect_state "and the list is the name" matches 1
	before=$(mstate line)
	do_input key Tab
	expect_state "and Tab again changes nothing" line "$before"
	mrun_quit
done

mrun_start
type_text "alpha x; beta y"
do_input key Return
mrun_exits "shell syntax"
wait_for "both ran" eval '[ -s "$MARKS/alpha" ] && [ -s "$MARKS/beta" ]'
[ "$(cat "$MARKS/alpha") $(cat "$MARKS/beta")" = "x y" ] ||
	fail "shell syntax: got '$(cat "$MARKS/alpha")' and '$(cat "$MARKS/beta")'"
rm "$MARKS/alpha" "$MARKS/beta"
echo "ok: a line with a blank is shell syntax"

mrun_start "$PROGS" "$A"
type_text ./alpha
do_input key Return
expect_ran "a path without a blank runs that file" alpha ""

mrun_start "$PROGS" "$A"
type_text ./nosuch
do_input key Return
still_open "a path that isn't there leaves it open"
expect_state "and says why" status "cannot start: No such file or directory"
mrun_quit

mrun_start "$Q:$A"
type_text "-dash x"
do_input key Return
expect_ran "a line starting with - is a command, not shell options" dash "x"

mrun_start "$Q:$A"
type_text x
do_input key Tab
type_text a
do_input key Return
expect_ran "Tab quotes a name with =, not an assignment" xy "a"

mrun_start "$Q:$A"
type_text if
do_input key Tab
type_text a
do_input key Return
expect_ran "and a reserved word" ifword "a"

mrun_start
type_text alp
do_input key Down
type_text " x"
do_input key Tab
expect_state "typing the arguments keeps what is chosen" line "alphabet x"
mrun_quit

# ---- editing -------------------------------------------------------------------

mrun_start
type_text alphx
do_input key BackSpace
do_input key Return
expect_ran "Backspace" alpha ""

mrun_start
type_text alpa
do_input key Left
type_text h
do_input key Return
expect_ran "Left moves the cursor" alpha ""

mrun_start
type_text alpa
do_input key Left
do_input key Right
do_input key BackSpace
type_text ha
do_input key Return
expect_ran "and Right back" alpha ""

mrun_start
type_text beta
do_input key ctrl+u
type_text alpha
do_input key Return
expect_ran "Ctrl+U clears" alpha ""

mrun_start
type_text xalpha
do_input key Home
do_input key Delete
do_input key Return
expect_ran "Home and Delete" alpha ""

mrun_start
type_text alph
do_input key Home
do_input key End
type_text a
do_input key KP_Enter
expect_ran "End, and Enter on the keypad" alpha ""

mrun_start
type_text alp
do_input key Down
do_input key shift+Tab
expect_state "Shift+Tab takes the name too" line "alphabet "
mrun_quit

mrun_start
do_input key Next
do_input key Next
mrun_sync || fail "Page Down: mrun ended"
[ "$(mstate sel)" = "$(mstate visible)" ] ||
	fail "Page Down: sel $(mstate sel), $(mstate visible) rows visible"
echo "ok: Page Down moves by the rows visible"
do_input key Prior
expect_state "and Page Up back" sel 0
mrun_quit

# Names in another case sort next to each other: the same name twice is listed once,
# and the one in the case typed goes first.
mrun_start
type_text gamma
do_input key Return
expect_ran "the name as typed before another case" lowgamma ""
mrun_start
do_input key shift+g
type_text amma
do_input key Return
expect_ran "and the other way round" Gamma ""
mrun_start
type_text gam
do_input key Down
do_input key Down
do_input key Down
do_input key Return
expect_ran "the same name in another case is listed once" lowgamma ""

# A compose sequence of its own, from plain keys: q then a is "alpha".
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
do_input key q
do_input key a
do_input key Return
expect_ran "a compose sequence" alpha ""
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
do_input key q
do_input key Escape
mrun_exits "Esc in the middle of a compose sequence"
echo "ok: Esc in the middle of a compose sequence still ends it"

# Characters of more than one byte: q then e is "é".
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
type_text qe
do_input key BackSpace
type_text alpha
do_input key Return
expect_ran "Backspace takes a character of two bytes" alpha ""
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
type_text alphqe
do_input key Left
do_input key Right
do_input key BackSpace
type_text a
do_input key Return
expect_ran "Left and Right step over it" alpha ""
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
type_text qealpha
do_input key Home
do_input key Delete
do_input key Return
expect_ran "and Delete takes it" alpha ""

XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start "$Q:$A"
type_text qetqe
do_input key Return
expect_ran "a name that isn't ASCII" ete ""
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start "$Q:$A"
type_text qetqe
do_input key Tab
type_text x
do_input key Return
expect_ran "is quoted by Tab" ete "x"
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start "$Q:$A"
type_text qEtqe
do_input key Return
expect_ran "and is found in another case" ete ""

# The status line is cut to its size on a character, not in one: after "a" or "ab".
for start in a ab; do
	XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
	type_text "$start"
	for ((i = 0; i < 13; i++)); do type_text qu; done
	do_input key Return
	mrun_sync || fail "a long name not found: mrun ended"
	[ -n "$(mstate status)" ] && mstate status | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1 ||
		fail "the status line after '$start' is not UTF-8: $(mstate status | od -c | tail -3)"
	mrun_quit
done
echo "ok: a long status is cut on a character"

# A held key repeats, and stops when the compositor turns repeating off.
mkdir -p "$XDG_CONFIG_HOME/mwc"
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
type_text qxqxqxqx
mrun_sync || fail "a long line: mrun ended"
len() { local l; l=$(mstate line); echo "${#l}"; }
[ "$(len)" = 252 ] || fail "a long line has $(len) bytes"
do_input press BackSpace
wait_for "a held Backspace repeats" eval '[ "$(len)" -lt 240 ]'
echo 'Mwm*keyboardRepeatRate: 0' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
# Settled once it hasn't changed for a fifth of a second (25 a second before), then
# it must stay so for longer than a repeat once a second would take.
for ((i = 0, n = -1; i < 50; i++)); do
	m=$(len)
	[ "$m" = "$n" ] && break
	n=$m
	sleep 0.2
done
for ((i = 0; i < 15; i++)); do
	[ "$(len)" = "$n" ] || fail "repeating is off, and Backspace still took $((n - $(len)))"
	sleep 0.1
done
do_input lift BackSpace
[ "$n" -gt 0 ] || fail "the line was empty before repeating was turned off"
echo "ok: rate 0 stops a key that is repeating"
mrun_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc

# ---- the mouse -----------------------------------------------------------------

mrun_start
type_text al
mrun_draws >/dev/null
# Until the chosen row is on screen, then measured in that one screenshot.
row_shown() {
	WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/s.png" &&
		[ "$(build/pngtool count "$SMOKE_DIR/s.png" $((CX + 200)) "$CY" 1 120 "$SEL" 0)" -gt 0 ]
}
wait_for "the chosen row is drawn" row_shown
for ((y = CY; y < CY + 120; y++)); do
	[ "$(build/pngtool pixel "$SMOKE_DIR/s.png" $((CX + 200)) "$y")" = "$SEL" ] && break
done
[ "$y" -lt $((CY + 120)) ] || fail "no chosen row found"
for ((end = y; end < y + 60; end++)); do
	[ "$(build/pngtool pixel "$SMOKE_DIR/s.png" $((CX + 200)) "$end")" = "$SEL" ] || break
done
# In the window: mwc places each new one somewhere else. The colour is inside the
# chosen row's raised bevel, which is 2 wide.
ROW0=$((y - CY - 2))
ROWH=$((end - y + 4))
echo "rows start at $ROW0, $ROWH high"
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
pixel_is "a click chooses" $((CX + 200)) $((CY + ROW0 + ROWH + ROWH / 2)) "$SEL"
# The chosen name on the active colour, a mid-tone: black reads better on it than white.
# Inside the bevel, whose light side would count as white.
black=$(build/pngtool count "$SMOKE_DIR/s.png" $((CX + 12)) $((CY + ROW0 + 2)) \
	150 $((ROWH - 4)) 000000)
white=$(build/pngtool count "$SMOKE_DIR/s.png" $((CX + 12)) $((CY + ROW0 + 2)) \
	150 $((ROWH - 4)) ffffff)
[ "$black" -gt 0 ] && [ "$white" = 0 ] ||
	fail "the chosen name has $black black and $white white pixels"
echo "ok: the chosen name is black"
do_input key Return
expect_ran "and Return runs that" alphabet ""

mrun_start
type_text al
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
do_input click left
expect_ran "a double click runs" alphabet ""

mrun_start
type_text "al x"
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
do_input click left
expect_ran "a double click keeps the arguments" alphabet "x"

mrun_start
type_text al
do_input move $((CX + 100)) $((CY + ROW0 + ROWH / 2))
do_input click left
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
still_open "two clicks on two rows are no double click"
expect_state "the second chooses" sel 1
mrun_quit

# A line as long as it can be: "al" and 4086 or 4087 x after the blank. With
# "alphabet" for "al" the shorter still fits, the longer doesn't.
long_line() { # <the last sequence>
	type_text "al "
	for ((i = 0; i < 64; i++)); do type_text qx; done
	type_text "$1"
}
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
long_line qbx
expect_state "a line of 4090 bytes" line "al $(printf '%04087d' 0 | tr 0 x)"
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
do_input click left
still_open "a double click with no room for the name runs nothing"
expect_state "and says so" status "the line has no room for the name"
mrun_quit
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
long_line qb
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
do_input click left
expect_ran "a name that just fits is taken" alphabet "$(printf '%04086d' 0 | tr 0 x)"

mrun_start
type_text alp
do_input move $((CX + 100)) $((CY + ROW0 + 200))
do_input click left
do_input click left
still_open "a double click below the rows does nothing"
do_input key ctrl+u
mrun_sync || fail "Ctrl+U: mrun ended"
[ "$(mstate matches)" -gt "$(mstate visible)" ] || fail "too few names to scroll"
do_input move $((CX + 100)) $((CY + ROW0 + ROWH))
do_input wheel 1
expect_state "the wheel scrolls" top 3
do_input wheel -1
expect_state "and back" top 0
still_open "and only scrolls"
[ "$(mstate sel)" = -1 ] || fail "the wheel chose $(mstate sel)"
do_input wheel 1
expect_state "the wheel again" top 3
# Another window takes the focus and gives it back: configures of the same size.
second_mrun
second_mrun_end
expect_state "and is still scrolled" top 3
# M2: the wheel stops at the end, and a turn past it draws nothing.
do_input wheel 20
mrun_sync || fail "the wheel to the end: mrun ended"
[ "$(mstate top)" = $(($(mstate matches) - $(mstate visible))) ] ||
	fail "the wheel at the end: top $(mstate top), $(mstate matches) matches, $(mstate visible) visible"
n=$(mrun_draws)
do_input wheel 1
[ "$(mrun_draws)" = "$n" ] || fail "a turn past the end drew"
echo "ok: the wheel stops at the end"
mrun_quit

# Losing the keyboard stops a key that is repeating.
XCOMPOSEFILE=$SMOKE_DIR/Compose mrun_start
type_text qxqxqxqx
mrun_sync || fail "a long line: mrun ended"
do_input press BackSpace
wait_for "a held Backspace repeats" eval '[ "$(len)" -lt 240 ]'
second_mrun
n=$(len)
for ((i = 0; i < 15; i++)); do
	[ "$(len)" = "$n" ] || fail "without the keyboard Backspace still took $((n - $(len)))"
	sleep 0.1
done
do_input lift BackSpace
second_mrun_end
echo "ok: losing the keyboard stops a repeating key"
mrun_quit

# A click, a new list, a click on the same row: two clicks on two programs.
mrun_start
type_text al
do_input move $((CX + 100)) $((CY + ROW0 + ROWH / 2))
do_input click left
do_input key ctrl+u
type_text b
do_input click left
still_open "two clicks on one row of two lists are no double click"
mrun_quit

# Two clicks on one row, slower than a double click.
mrun_start
type_text al
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
mrun_sync || fail "a slow double click: mrun ended"
sleep 0.5       # the time between the clicks is what is tested
do_input click left
still_open "two slow clicks are no double click"
expect_state "and choose" sel 1
# Nor other buttons.
for b in right middle; do
	do_input move $((CX + 100)) $((CY + ROW0 + ROWH / 2))
	do_input click "$b"
	do_input click "$b"
done
still_open "right and middle double clicks run nothing"
expect_state "and choose nothing" sel 1
mrun_quit

# The keys keep the chosen row in view.
mrun_start
do_input key Up
mrun_sync || fail "Up: mrun ended"
[ "$(mstate top)" = $(($(mstate matches) - $(mstate visible))) ] ||
	fail "Up to the last: top $(mstate top), $(mstate matches) matches, $(mstate visible) visible"
echo "ok: Up to the last shows it"
mrun_quit
mrun_start
do_input key Down
mrun_sync || fail "Down: mrun ended"
for ((i = 0; i < $(mstate visible); i++)); do do_input key Down; done
expect_state "Down past the rows shown scrolls by one" top 1

# Typing clears an error.
do_input key ctrl+u
type_text nosuch
do_input key Return
expect_state "an error in the status line" status 'not in $PATH: nosuch'
do_input key BackSpace
expect_state "goes with the next key" status ""
mrun_quit

# Closed by the compositor.
mrun_start
do_input key alt+F4
mrun_exits "closed by mwc"
echo "ok: closed by mwc"

# Maximized it shows more rows, and back it has its size again.
mrun_start
refresh
SIZE="$(get mrun w) $(get mrun h)"
mrun_sync || fail "maximize: mrun ended"
ROWS=$(mstate visible)
do_input key alt+F10
check "maximized" '[ "$(get mrun max)" = 1 ]'
mrun_sync || fail "maximized: mrun ended"
[ "$(mstate visible)" -gt "$ROWS" ] || fail "maximized it shows $(mstate visible) rows, not more than $ROWS"
do_input key alt+F10
check "and back to its size" '[ "$(get mrun w) $(get mrun h)" = "$SIZE" ]'
expect_state "and its rows" visible "$ROWS"
mrun_quit

# A name that starts right to left is drawn from the left, as the others.
H=$SMOKE_DIR/bin-h
mkdir -p "$H"
program "$H" "שלום" shalom
mrun_start "$H"
do_input key Down
mrun_draws >/dev/null
pixel_is "the right to left name is chosen" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) "$SEL"
black=$(build/pngtool count "$SMOKE_DIR/shot.png" $((CX + 12)) $((CY + ROW0 + 2)) 60 $((ROWH - 4)) 000000)
[ "$black" -gt 0 ] || fail "nothing of the right to left name at the left of its row"
echo "ok: a right to left name is drawn from the left"
mrun_quit

# Keys and wheel turns that change nothing draw nothing.
mrun_start
type_text alp
do_input key Down
do_input move $((CX + 100)) $((CY + ROW0 + ROWH))
n=$(mrun_draws)
do_input key Down
do_input key Right
do_input key End
do_input wheel -1
[ "$(mrun_draws)" = "$n" ] || fail "nothing changed, and mrun drew $(($(mrun_draws) - n)) frames"
echo "ok: what changes nothing draws nothing"
mrun_quit

# ---- starting ------------------------------------------------------------------

# Removed after mrun read the list: the exec fails, and mrun stays to say so.
mrun_start
rm "$A/gone"
type_text gone
do_input key Return
still_open "a program that can't start leaves it open"
expect_state "and says why" status "cannot start: No such file or directory"
do_input key ctrl+u
type_text alpha
do_input key Return
expect_ran "and a program after it starts" alpha ""

# Started with stderr closed: the error of the child doesn't go into the connection.
program "$A" gone
mrun_start "$PROGS" . nostderr
rm "$A/gone"
type_text gone
do_input key Return
still_open "with stderr closed it stays connected"
expect_state "and says why" status "cannot start: No such file or directory"
mrun_quit

# With stderr a pipe nobody reads, the child's error still comes back.
program "$A" gone
mrun_start "$PROGS" . brokenstderr
rm "$A/gone"
type_text gone
do_input key Return
still_open "with stderr a broken pipe it stays open"
expect_state "and says why" status "cannot start: No such file or directory"
mrun_quit

# MRUN_TEST_STATE set but empty is not set.
mkdir -p "$SMOKE_DIR/nostate"
(cd "$SMOKE_DIR/nostate" && PATH=$PROGS WAYLAND_DISPLAY=wayland-0 MRUN_TEST_STATE= \
	exec "$MRUN") >>"$MRUN_LOG" 2>&1 &
MRUN_PID=$!
check "mrun with an empty MRUN_TEST_STATE" '[ "$(get mrun kbfocus)" = 1 ]'
mrun_quit
[ -z "$(ls -A "$SMOKE_DIR/nostate")" ] || fail "an empty MRUN_TEST_STATE wrote $(ls -A "$SMOKE_DIR/nostate")"
echo "ok: an empty MRUN_TEST_STATE writes nothing"

# A socket that is no compositor: it closes the connection at once.
python3 -c 'import socket, sys
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
s.listen()
while True:
	s.accept()[0].close()' "$SMOKE_DIR/notwayland" &
FAKE_PID=$!
wait_for "a socket that isn't Wayland" test -S "$SMOKE_DIR/notwayland"
err=$(WAYLAND_DISPLAY=$SMOKE_DIR/notwayland "$MRUN" 2>&1)
kill "$FAKE_PID"
case $err in
*"lost the connection"*) echo "ok: a connection closed at once is reported as that" ;;
*) fail "a connection closed at once: $err" ;;
esac

# ---- ending --------------------------------------------------------------------

mrun_start
do_input key alt+q
do_input key super+w
still_open "alt+q and super+w are not for it"
do_input key ctrl+q
mrun_exits "Ctrl+Q"
mrun_start
do_input key ctrl+w
mrun_exits "Ctrl+W"
echo "ok: Ctrl+Q and Ctrl+W end it"

# In a layout without Latin letters the shortcuts are the keys of the first one.
mrun_start
type_text beta
do_input group 1
do_input key u
expect_state "the second layout types Cyrillic" line "betaг"
do_input key ctrl+u
expect_state "Ctrl+U clears in it" line ""
do_input key ctrl+q
mrun_exits "Ctrl+Q in it"
do_input group 0
echo "ok: Ctrl+U and Ctrl+Q in a layout without Latin letters"

# What it starts goes on without it, in a session of its own, without its input.
mrun_start
type_text omega
do_input key Return
mrun_exits "starting omega"
wait_for "omega is running" test -s "$MARKS/omega"
OMEGA=$(cat "$MARKS/omega")
kill -0 "$OMEGA" 2>/dev/null || fail "what it started ended with it"
[ "$(readlink "/proc/$OMEGA/fd/0")" = /dev/null ] || fail "its input isn't /dev/null"
[ "$(ps -o sid= -p "$OMEGA" | tr -d ' ')" = "$(ps -o pid= -p "$OMEGA" | tr -d ' ')" ] ||
	fail "it has no session of its own"
kill "$OMEGA"
rm "$MARKS/omega"
echo "ok: it goes on by itself"

# ---- mwmrc -----------------------------------------------------------------------

# The environment before mwmrc, and only what is a colour: rgb: takes hex digits.
MWC_ACTIVE='#00c000' MWC_INACTIVE='#806040' mrun_start
type_text al
pixel_is "MWC_ACTIVE" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) 00c000
pixel_is "MWC_INACTIVE" $((CX + 1)) $((CY + 1)) 806040
mrun_quit
MWC_ACTIVE='rgb:-1/0/0' mrun_start
type_text al
pixel_is "rgb: with a sign is no colour" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) "$SEL"
mrun_quit
for c in rgb:0/c/0=00cc00 rgb:0/ffff/0=00ff00 rgb:00/80/0=008000; do
	MWC_ACTIVE=${c%=*} mrun_start
	type_text al
	pixel_is "${c%=*}" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) "${c#*=}"
	mrun_quit
done

# MRUN_FONT: a bigger one shows fewer rows; one without a size gets 10 points.
mrun_start
mrun_sync || fail "the default font: mrun ended"
ROWS=$(mstate visible)
mrun_quit
MRUN_FONT='Sans 20' mrun_start
mrun_sync || fail "MRUN_FONT: mrun ended"
[ "$(mstate visible)" -lt "$ROWS" ] || fail "MRUN_FONT 'Sans 20' shows $(mstate visible) rows, $ROWS with the default"
echo "ok: a bigger MRUN_FONT shows fewer rows"
mrun_quit
MRUN_FONT=Sans mrun_start
expect_state "MRUN_FONT without a size is 10 points" visible "$ROWS"
mrun_quit

# From mwmrc, and the last line that says it; the rest of a line too long to read
# in one isn't a line of its own.
mkdir -p "$XDG_CONFIG_HOME/mwc"
{
	echo 'Mwm*activeBackground: #0000c0'
	echo 'Mwm*background: #806040'
	echo 'Mwm*activeBackground: #00c000'
	printf 'Mwm*title: %0500d' 0   # 511 bytes, as much as mrun reads at once
	echo 'Mwm*activeBackground: #c00000'
} >"$XDG_CONFIG_HOME/mwc/mwmrc"
mrun_start
type_text al
pixel_is "Mwm*activeBackground, the last line" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) 00c000
pixel_is "Mwm*background" $((CX + 1)) $((CY + 1)) 806040
mrun_quit
# The environment goes before it.
MWC_ACTIVE='#c0c000' MWC_INACTIVE='#405060' mrun_start
type_text al
pixel_is "MWC_ACTIVE before mwmrc" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) c0c000
pixel_is "MWC_INACTIVE before mwmrc" $((CX + 1)) $((CY + 1)) 405060
mrun_quit

# Mwm*foreground is the text, as in mwc: on the chosen row and the others alike.
echo 'Mwm*foreground: #c00000' >"$XDG_CONFIG_HOME/mwc/mwmrc"
mrun_start
type_text al
pixel_is "the chosen row with a foreground set" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) "$SEL"
for row in 0 1; do
	ry=$((CY + ROW0 + row * ROWH + 2))
	red=$(build/pngtool count "$SMOKE_DIR/shot.png" $((CX + 12)) "$ry" 150 $((ROWH - 4)) c00000)
	black=$(build/pngtool count "$SMOKE_DIR/shot.png" $((CX + 12)) "$ry" 150 $((ROWH - 4)) 000000)
	[ "$red" -gt 0 ] && [ "$black" = 0 ] ||
		fail "Mwm*foreground: row $row has $red red and $black black pixels"
done
echo "ok: Mwm*foreground is the text"
mrun_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# ---- a scaled output ------------------------------------------------------------

# At scale 2 it draws twice the pixels: the first line of the chosen row's bevel and
# of its colour are each theirs, not a blend from a buffer scaled up.
mkdir -p "$XDG_CONFIG_HOME/mwc"
echo 'Mwm*HEADLESS-1*outputScale: 2' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
mrun_start
type_text al
pixel_is "sharp at scale 2: the row's bevel" $((2 * (CX + 200))) $((2 * (CY + ROW0))) "$TOP"
pixel_is "and its colour" $((2 * (CX + 200))) $((2 * (CY + ROW0 + 2))) "$SEL"

# The cursor comes at the window's scale, and in a buffer scale that divides its size:
# Adwaita has 24, 30, 36, 48, 72 and 96 pixels, so at 5 it gives 96 for 24, and a
# buffer scale of 5 would be a protocol error. A scale change reloads the cursor with
# the pointer on the window, not only at the next enter.
cursor_ok='[ "$(global cursor cw)" -gt 0 ] && [ $(($(global cursor cw) % $(global cursor cscale))) = 0 ] &&
	[ $(($(global cursor ch) % $(global cursor cscale))) = 0 ]'
do_input move $((2 * (CX + 100))) $((2 * (CY + 40)))   # vinput counts in pixels
check "the cursor at scale 2" "$cursor_ok && [ \"\$(global cursor cscale)\" = 2 ]"
expect_state "the theme is loaded once, at that scale" cursors 1
CW2=$(global cursor cw)
sed -i 's/outputScale: 2/outputScale: 5/' "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 5" "[ \"\$(global output_HEADLESS-1 scale)\" = 500 ]"
on_mrun='[ $(global cursor x) -ge $(($(get mrun x) + B)) ] &&
	[ $(global cursor x) -lt $(($(get mrun x) + B + $(get mrun w))) ] &&
	[ $(global cursor y) -ge $(($(get mrun y) + T + B)) ] &&
	[ $(global cursor y) -lt $(($(get mrun y) + T + B + $(get mrun h))) ]'
check "the cursor is reloaded with the pointer still on it" \
	"$cursor_ok && [ \"\$(global cursor cw)\" != $CW2 ] && $on_mrun"
echo "cursor at scale 5: $(global cursor cw)x$(global cursor ch)@$(global cursor cscale)"
# At 5 the window covers the output: a new one is entered at that scale.
mrun_quit
mrun_start
do_input move $((5 * (CX + 20))) $((5 * (CY + 20)))
check "a new window's cursor at scale 5 has a buffer scale that divides it" "$cursor_ok"
mrun_quit

[ "$("$MRUN" -v)" = "mrun $(sed -n 's/^#define MRUN_VERSION "\(.*\)"/\1/p' "$MRUN_DIR/version.h")" ] ||
	fail "-v: $("$MRUN" -v)"
"$MRUN" -v >/dev/null || fail "-v exits with $?"
echo "ok: -v prints the version"
"$MRUN" -h 2>/dev/null || fail "-h exits with $?"
"$MRUN" -x 2>/dev/null; [ $? = 1 ] || fail "an unknown option doesn't exit with 1"
echo "ok: -h, and an unknown option"

trap cleanup EXIT
stop_vinput
stop_mwc
echo "PASS: ui"
