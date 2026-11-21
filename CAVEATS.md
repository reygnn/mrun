# Caveats

Limits of mrun that are known and left as they are.


## Starting programs

A line with a blank goes to the shell, and so does a path with anything but ASCII
letters, digits and `_+-.,:@%/`, such as `~`, `$` or a letter that isn't ASCII. mrun quits right after starting the shell; whether the
command really started, mrun can't see.

The list is read once at the start; a program installed while mrun is open is not in
it.

Entries in `$PATH` that are not absolute don't count, except for the shell, which gets
`$PATH` as it is.

Names that are not valid UTF-8 or contain control characters are not listed. With a
path and a blank they can still be started through the shell.

A directory in `$PATH` that can be searched but not read, such as one with mode 0711,
can't be listed, so its programs aren't offered. The shell still finds them on a line
with a blank.



## Drawing

The status line is drawn between the background and the text colour, darker than
mwc's dimmed menu items: it is a hint to be read, not something that can't be
chosen.

The chosen row is the active colour with a raised bevel, as an armed menu item in mwc,
whose text keeps the menu's colour. Unless `Mwm*foreground` is set it is black or white
by Motif's rule, black on mwc's default light stone. A mid-tone set as the colour can
fall short of WCAG's 4.5:1: on Motif's old focused blue `#7387ad` it is white at about
3.6:1.

It needs wl_compositor 4. It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.
