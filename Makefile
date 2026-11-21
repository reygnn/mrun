# mrun - minimal run dialog for Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
MTK_DIR = mtk

all: mrun

include $(MTK_DIR)/mtk.mk

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g

mrun: mrun.c version.h build/gitversion.h $(MTK_DEPS)
	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mrun.c $(MTK_SRC) \
		$(MTK_LIBS) $(LDLIBS)

build:
	mkdir -p build

# The commit a build is from, after the version: +DATE.TIME.HASH, its commit time in
# UTC, and .dirty for changes not committed. Rewritten only when it changes, so
# mrun is linked again only then; empty without git.
build/gitversion.h: FORCE | build
	@v=$$(TZ=UTC0 git log -1 --format=%cd.%h --date=format-local:%Y%m%d.%H%M 2>/dev/null); \
	if [ -n "$$v" ] && ! git diff --quiet HEAD 2>/dev/null; then v=$$v.dirty; fi; \
	line="#define MRUN_GIT \"$${v:++$$v}\""; \
	[ "$$(cat $@ 2>/dev/null)" = "$$line" ] || echo "$$line" >$@

FORCE:

install: mrun
	install -Dm755 mrun $(DESTDIR)$(PREFIX)/bin/mrun

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mrun

# Needs a built mwc next to this directory, or in MWC_DIR, and grim.
test: mrun
	tests/ui.sh

clean:
	rm -rf mrun build

.PHONY: all install uninstall test clean FORCE
