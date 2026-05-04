# C Stuff
CC ?= cc
VER = 2.26
GIT_VER != git describe --always --tags 2>/dev/null || echo unknown
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -DGIT_VER=\"$(GIT_VER)\" -DVERSION=\"$(VER)\"
CFLAGS = -std=c99 -Wall -Wextra -Iserver/include -Iexternal -pthread

OS_TYPE := $(shell uname -s)
ifeq ($(OS_TYPE),Linux)
    SRC = server/*.c
    LDFLAGS = -lpthread
else
    SRC = server/*.c external/tinycthread.c
    LDFLAGS =
endif

OUT = parados

# Install Paths
ETCDIR  ?= /etc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/man
MAN1DIR ?= $(MANDIR)/man1
MAN5DIR ?= $(MANDIR)/man5
MAN7DIR ?= $(MANDIR)/man7

all: release

release:
	$(CC) $(CPPFLAGS) -DNDEBUG $(CFLAGS) -O2 $(SRC) $(LDFLAGS) -o $(OUT)

debug:
	$(CC) $(CPPFLAGS) -DDEBUG $(CFLAGS) -g $(SRC) $(LDFLAGS) -o $(OUT)

clean:
	rm -f $(OUT)

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	cp $(OUT) $(DESTDIR)$(BINDIR)/$(OUT)
	chmod 755 $(DESTDIR)$(BINDIR)/$(OUT)

	mkdir -p $(DESTDIR)$(MAN1DIR)
	mkdir -p $(DESTDIR)$(MAN5DIR)
	mkdir -p $(DESTDIR)$(MAN7DIR)

	cp docs/parados.1 $(DESTDIR)$(MAN1DIR)/parados.1
	cp docs/parados.conf.5 $(DESTDIR)$(MAN5DIR)/parados.conf.5
	cp docs/parados.7 $(DESTDIR)$(MAN7DIR)/parados.7

	chmod 644 $(DESTDIR)$(MAN1DIR)/parados.1
	chmod 644 $(DESTDIR)$(MAN5DIR)/parados.conf.5
	chmod 644 $(DESTDIR)$(MAN7DIR)/parados.7

install-conf:
	mkdir -p $(DESTDIR)$(ETCDIR)
	cp parados.conf $(DESTDIR)$(ETCDIR)/parados.conf
	chmod 644 $(DESTDIR)$(ETCDIR)/parados.conf

install-rcctl:
	install -m 0555 scripts/openbsd-parados.sh /etc/rc.d/parados

install-openrc:
	install -m 0555 scripts/openrc-parados /etc/init.d/parados

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(OUT)
	rm -f $(DESTDIR)$(MAN1DIR)/parados.1
	rm -f $(DESTDIR)$(MAN5DIR)/parados.conf.5
	rm -f $(DESTDIR)$(MAN7DIR)/parados.7

compile_flags:
	rm -f compile_flags.txt
	for f in ${CPPFLAGS} ${CFLAGS}; do echo $$f >> compile_flags.txt; done

.PHONY: all release debug clean install install-conf install-rcctl install-openrc uninstall compile_flags

