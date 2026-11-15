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
PROGS=$A:$Z

mrun_start() { # [PATH [directory]]
	(cd "${2:-.}" && PATH=${1:-$PROGS} WAYLAND_DISPLAY=wayland-0 exec "$MRUN") \
		>>"$MRUN_LOG" 2>&1 &
	MRUN_PID=$!
	check "mrun has a focused window" '[ "$(get mrun kbfocus)" = 1 ]'
	CX=$(($(get mrun x) + B))
	CY=$(($(get mrun y) + T + B))
}

type_text() {
	local i c
	for ((i = 0; i < ${#1}; i++)); do
		c=${1:i:1}
		[ "$c" = " " ] && c=space
		do_input key "$c"
	done
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
	for ((i = 0; i < 50; i++)); do
		[ -e "$MARKS/$2" ] && break
		sleep 0.1
	done
	[ -e "$MARKS/$2" ] || fail "$1: $2 didn't run (ran: $(ls "$MARKS" | xargs))"
	[ "$(cat "$MARKS/$2")" = "$3" ] || fail "$1: $2 got '$(cat "$MARKS/$2")', not '$3'"
	rm "$MARKS/$2"
	[ -z "$(ls "$MARKS")" ] || fail "$1: also ran $(ls "$MARKS" | xargs)"
	echo "ok: $1"
}

still_open() { # <description>: nothing ran, and it is there
	sleep 0.3
	kill -0 "$MRUN_PID" 2>/dev/null || fail "$1: mrun ended"
	[ -z "$(ls "$MARKS")" ] || fail "$1: ran $(ls "$MARKS" | xargs)"
	echo "ok: $1"
}

mrun_quit() {
	do_input key Escape
	mrun_exits "Esc"
}

trap 'WAYLAND_DISPLAY=wayland-0 grim "$MRUN_DIR/build/ui-last.png" 2>/dev/null; cleanup' EXIT

start_mwc 'touch "$SMOKE_DIR/up"; exec sleep 600'
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput
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

# ---- what isn't a program -------------------------------------------------------

for name in nothing delta epsilon hidden; do
	mrun_start
	type_text "$name"
	do_input key Return
	still_open "$name isn't offered"
	mrun_quit
done

mrun_start
do_input key Return
still_open "Return on an empty line does nothing"
do_input key Down
do_input key Return
expect_ran "but Down chooses the first of all" alpha ""

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
printf '<q> <a> : "alpha"\n' >"$SMOKE_DIR/Compose"
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

# ---- the mouse -----------------------------------------------------------------

mrun_start
type_text al
for ((y = CY; y < CY + 120; y++)); do
	WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/s.png" || fail "no screenshot"
	[ "$(build/pngtool pixel "$SMOKE_DIR/s.png" $((CX + 200)) "$y")" = "$SEL" ] && break
done
[ "$y" -lt $((CY + 120)) ] || fail "no chosen row found"
for ((end = y; end < y + 60; end++)); do
	[ "$(build/pngtool pixel "$SMOKE_DIR/s.png" $((CX + 200)) "$end")" = "$SEL" ] || break
done
# In the window: mwc places each new one somewhere else.
ROW0=$((y - CY))
ROWH=$((end - y))
echo "rows start at $ROW0, $ROWH high"
do_input move $((CX + 100)) $((CY + ROW0 + ROWH + ROWH / 2))
do_input click left
pixel_is "a click chooses" $((CX + 200)) $((CY + ROW0 + ROWH + ROWH / 2)) "$SEL"
# The chosen name on the active colour, a mid-tone: black reads better on it than white.
black=$(build/pngtool count "$SMOKE_DIR/s.png" $((CX + 10)) $((CY + ROW0 + 1)) \
	150 $((ROWH - 2)) 000000)
white=$(build/pngtool count "$SMOKE_DIR/s.png" $((CX + 10)) $((CY + ROW0 + 1)) \
	150 $((ROWH - 2)) ffffff)
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
do_input move $((CX + 100)) $((CY + ROW0 + 200))
do_input click left
do_input click left
still_open "a double click below the rows does nothing"
do_input wheel 1
do_input wheel -1
still_open "the wheel only scrolls"
mrun_quit

# ---- starting ------------------------------------------------------------------

# Removed after mrun read the list: the exec fails, and mrun stays to say so.
mrun_start
rm "$A/gone"
type_text gone
do_input key Return
still_open "a program that can't start leaves it open"
mrun_quit

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

# Mwm*foreground is the text, as in mwc: on the chosen row and the others alike.
mkdir -p "$XDG_CONFIG_HOME/mwc"
echo 'Mwm*foreground: #c00000' >"$XDG_CONFIG_HOME/mwc/mwmrc"
mrun_start
type_text al
pixel_is "the chosen row with a foreground set" $((CX + 200)) $((CY + ROW0 + ROWH / 2)) "$SEL"
for row in 0 1; do
	ry=$((CY + ROW0 + row * ROWH + 1))
	red=$(build/pngtool count "$SMOKE_DIR/shot.png" $((CX + 10)) "$ry" 150 $((ROWH - 2)) c00000)
	black=$(build/pngtool count "$SMOKE_DIR/shot.png" $((CX + 10)) "$ry" 150 $((ROWH - 2)) 000000)
	[ "$red" -gt 0 ] && [ "$black" = 0 ] ||
		fail "Mwm*foreground: row $row has $red red and $black black pixels"
done
echo "ok: Mwm*foreground is the text"
mrun_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# ---- a scaled output ------------------------------------------------------------

# At scale 2 it draws twice the pixels: the first and last line of the chosen row are
# its colour, not a blend with the list from a buffer scaled up.
mkdir -p "$XDG_CONFIG_HOME/mwc"
echo 'Mwm*HEADLESS-1*outputScale: 2' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
mrun_start
type_text al
pixel_is "sharp at scale 2: the row's first line" $((2 * (CX + 200))) $((2 * (CY + ROW0))) "$SEL"
pixel_is "and its last" $((2 * (CX + 200))) $((2 * (CY + ROW0 + ROWH) - 1)) "$SEL"

# The cursor comes at the window's scale, and in a buffer scale that divides its size:
# Adwaita has 24, 30, 36, 48, 72 and 96 pixels, so at 5 it gives 96 for 24, and a
# buffer scale of 5 would be a protocol error. A scale change reloads the cursor with
# the pointer on the window, not only at the next enter.
cursor_ok='[ "$(global cursor cw)" -gt 0 ] && [ $(($(global cursor cw) % $(global cursor cscale))) = 0 ] &&
	[ $(($(global cursor ch) % $(global cursor cscale))) = 0 ]'
do_input move $((2 * (CX + 100))) $((2 * (CY + 40)))   # vinput counts in pixels
check "the cursor at scale 2" "$cursor_ok && [ \"\$(global cursor cscale)\" = 2 ]"
CW2=$(global cursor cw)
sed -i 's/outputScale: 2/outputScale: 5/' "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 5" "[ \"\$(global output_HEADLESS-1 scale)\" = 500 ]"
check "the cursor is reloaded with the pointer still on it" \
	"$cursor_ok && [ \"\$(global cursor cw)\" != $CW2 ]"
echo "cursor at scale 5: $(global cursor cw)x$(global cursor ch)@$(global cursor cscale)"
# At 5 the window covers the output: a new one is entered at that scale.
mrun_quit
mrun_start
do_input move $((5 * (CX + 20))) $((5 * (CY + 20)))
check "a new window's cursor at scale 5 has a buffer scale that divides it" "$cursor_ok"
mrun_quit

[ "$("$MRUN" -v)" = "mrun $(sed -n 's/^#define MRUN_VERSION "\(.*\)"/\1/p' "$MRUN_DIR/version.h")" ] ||
	fail "-v: $("$MRUN" -v)"
echo "ok: -v prints the version"

trap cleanup EXIT
stop_vinput
stop_mwc
echo "PASS: ui"
