# mrun

mrun is a run dialog for Wayland in the look of the Motif window manager. It is made
for [mwc](https://github.com/reygnn/mwc), which opens it with the middle mouse button
on the desktop, and runs on any Wayland compositor with xdg-shell. One C file on
[mtk](https://github.com/reygnn/mtk), the toolkit of mwc's clients, vendored in `mtk/`:
libwayland, xkbcommon, Cairo and Pango.


## Build

On Ubuntu 26.04:

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install

`make install` puts `mrun` in `/usr/local/bin`.


## Run

    mrun

At the start mrun reads the programs in `$PATH`, and typing narrows the list: the exact
name first, as typed before in another case, then names starting with the text, then
names containing it, ignoring case.
It quits once it has started a program; one that can't be started leaves it open, with
the reason in the status line.

Return runs one of two things. A line without a blank runs the chosen program directly,
without a shell, so `fire` starts Firefox if that is the first match. Without a match
the status line says so, unless the line has a `/`; then that file is started if the
line has nothing but ASCII letters, digits and `_+-.,:@%/`, and anything else, such
as `~` or a letter that isn't ASCII, sends it to the shell. A line with a blank goes
to `/bin/sh -c` as it is, so arguments and shell syntax work, such as
`firefox --private-window` or `sleep 5; foot`. A blank in single quotes or after a
backslash is part of the name, as for the shell, so `'two words'` is a program. On a
line that is empty or only blanks Return does nothing until a program is chosen.

Tab and a double click with arguments put the name in single quotes if the shell would
read it otherwise, such as a name with a blank or `=`, or a word like `if`. Typing the
arguments keeps the program that is chosen.

Programs run in a session of their own, without a terminal: a terminal program such as
`htop` is started as `foot htop`.

`-v` prints the version, which is set in `version.h`,
and the commit built, as `VERSION+DATE.TIME.HASH` with its commit time in UTC
and `.dirty` for changes not committed.


## Keys

| Keys | Action |
|---|---|
| typing | narrow the list |
| Up, Down, Page Up, Page Down | choose a program |
| Return, Enter on the keypad | run |
| Tab, Shift+Tab | take the chosen name, with a blank after it for arguments |
| Left, Right, Home, End, Backspace, Delete | edit the line |
| Ctrl+U | clear the line |
| Esc, Ctrl+Q, Ctrl+W | quit |
| click, double click | choose, run |
| wheel | scroll the list |


## Configuration

| Variable | Default | |
|---|---|---|
| `MRUN_FONT` | `Sans 10` | Pango font name, e.g. `Sans Bold 11` |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aaaaaa`, `#c6c1aa` | background, chosen row |

The colours are mwc's: `MWC_INACTIVE` and `MWC_ACTIVE` from the environment, else
`Mwm*background` and `Mwm*activeBackground` from `$XDG_CONFIG_HOME/mwc/mwmrc` (by
default `~/.config/mwc/mwmrc`), else the Motif defaults. The list and the chosen row
are mtk's: the row is the active colour, moved towards black or white where it is too
close to the list to tell apart (Base's `#c6c1aa` on `#c4c4c4`). The text is
`Mwm*foreground` where it is set; else it reads on both, at 4.5:1.


## How this is built

mrun is developed with Claude (Anthropic) as a coding assistant. I set the scope and the
design and review the changes; Claude writes most of the code and the tests. The checks:

* `make test` drives mrun under a headless mwc with a simulated mouse and keyboard and
  checks with small test programs in its `PATH` what it started and with which
  arguments, as well as its colours and its drawing on a scaled output.
* Every fix comes with a test that fails without it, checked by taking the fix out.
* What is left open is written down rather than hidden: see CAVEATS, and the reviews
  in `docs/history`.


## Testing

    make test

runs mrun in a headless mwc with a simulated mouse and keyboard, and checks with small
test programs in its `PATH` what it started and with which arguments, as well as the
colours from the environment and mwmrc and its drawing on a scaled output. With
`MRUN_TEST_STATE` set, mrun writes its line, list and status to that file for the
test to read. It needs a built mwc with `build/vinput`
and `build/pngtool` in `MWC_DIR` (by default `../mwc`), and grim. mwc's development
box has all of it:

    distrobox enter mwc-dev -- make test


## License

MIT, see [LICENSE](LICENSE).
