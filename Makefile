# mrun - minimal run dialog for Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
MTK_DIR = mtk

all: mrun

include $(MTK_DIR)/mtk.mk

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g

mrun: mrun.c version.h $(MTK_DEPS)
	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mrun.c $(MTK_SRC) \
		$(MTK_LIBS) $(LDLIBS)

build:
	mkdir -p build

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
