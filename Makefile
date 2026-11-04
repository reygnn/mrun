# mrun - minimal run dialog for Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION = $(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

PROTO_H = build/xdg-shell-client-protocol.h \
	build/xdg-decoration-unstable-v1-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c build/xdg-decoration-unstable-v1-protocol.c

# CFLAGS and LDLIBS stay the user's, also on the command line: `make CFLAGS=-O0`
# must not lose the include paths.
CFLAGS ?= -O2 -g
MRUN_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
MRUN_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

all: mrun

mrun: mrun.c $(PROTO_H) $(PROTO_C)
	$(CC) $(MRUN_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mrun.c $(PROTO_C) $(MRUN_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

build/xdg-decoration-unstable-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_DECORATION) $@

build/xdg-decoration-unstable-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_DECORATION) $@

install: mrun
	install -Dm755 mrun $(DESTDIR)$(PREFIX)/bin/mrun

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mrun

# Needs a built mwc next to this directory, or in MWC_DIR, and grim.
test: mrun
	tests/ui.sh

clean:
	rm -rf mrun build

.PHONY: all install uninstall test clean
