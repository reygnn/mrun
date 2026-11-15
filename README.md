# mrun

mrun is a run dialog for Wayland in the look of the Motif window manager. It is made
for [mwc](https://github.com/reygnn/mwc), which opens it with the middle mouse button
on the desktop, and runs on any Wayland compositor with xdg-shell. One C file on
libwayland, xkbcommon, Cairo and Pango, no toolkit.


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
the status line says so, unless the line has a `/`; then it goes to the shell. A line
with a blank goes to `/bin/sh -c` as it is, so arguments and shell syntax work, such
as `firefox --private-window` or `sleep 5; foot`. On an empty line Return does nothing
until a program is chosen.

Programs run in a session of their own, without a terminal: a terminal program such as
`htop` is started as `foot htop`.

`-v` prints the version, which is set in `version.h`.


## Keys

| Keys | Action |
|---|---|
| typing | narrow the list |
| Up, Down, Page Up, Page Down | choose a program |
| Return | run |
| Tab | take the chosen name, with a blank after it for arguments |
| Left, Right, Home, End, Backspace, Delete | edit the line |
| Ctrl+U | clear the line |
| Esc, Ctrl+Q, Ctrl+W | quit |
| click, double click | choose, run |
| wheel | scroll the list |


## Configuration

| Variable | Default | |
|---|---|---|
| `MRUN_FONT` | `Sans 10` | Pango font name, e.g. `Sans Bold 11` |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | background, chosen row |

The colours are mwc's: `MWC_INACTIVE` and `MWC_ACTIVE` from the environment, else
`Mwm*background` and `Mwm*activeBackground` from `~/.config/mwc/mwmrc`, else the Motif
defaults. The text is `Mwm*foreground` where it is set, else black or white, whichever
reads better.


## Testing

    make test

runs mrun in a headless mwc with a simulated mouse and keyboard, and checks with small
test programs in its `PATH` what it started and with which arguments, as well as its
colours and its drawing on a scaled output. It needs a built mwc with `build/vinput`
and `build/pngtool` in `MWC_DIR` (by default `../mwc`), and grim. mwc's development
box has all of it:

    distrobox enter mwc-dev -- make test


## License

MIT, see [LICENSE](LICENSE).
