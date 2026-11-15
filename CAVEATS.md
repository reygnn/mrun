# Caveats

Limits of mrun that are known and left as they are.


## Starting programs

A line with a blank goes to the shell, and mrun quits right after starting it. Whether
the command really started, mrun can't see.

The list is read once at the start; a program installed while mrun is open is not in
it.

Entries in `$PATH` that are not absolute don't count, except for the shell, which gets
`$PATH` as it is.

Names that are not valid UTF-8 or contain control characters are not listed. With a
path and a blank they can still be started through the shell.


## Drawing

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.
