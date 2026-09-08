CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11

# Installation paths
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

# Program name - changed to zelpy
PROG = zelpy

$(PROG): wm.c animation.c animation.h
	$(CC) $(CFLAGS) -o $(PROG) wm.c animation.c $(LDFLAGS)

clean:
	rm -f $(PROG)

install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp -f $(PROG) $(DESTDIR)$(BINDIR)/
	chmod 755 $(DESTDIR)$(BINDIR)/$(PROG)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG)

.PHONY: clean install uninstall
