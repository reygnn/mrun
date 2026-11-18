# Caveats

Limits of mrun that are known and left as they are.


## Starting programs

A line with a blank goes to the shell, and so does a path with characters the shell
reads, such as `~` or `$`. mrun quits right after starting the shell; whether the
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


## Keys

Ctrl+U, Ctrl+Q and Ctrl+W go by the keysym of the active layout: with a layout such
as Russian active they do nothing. Esc always quits.


## Drawing

The status line is drawn between the background and the text colour, darker than
mwc's dimmed menu items: it is a hint to be read, not something that can't be
chosen.

The chosen row is the active colour with a raised bevel, as an armed menu item in mwc,
whose text keeps the menu's colour. mrun picks black or white for it by contrast
unless `Mwm*foreground` is set.

It needs wl_compositor 4. It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.
