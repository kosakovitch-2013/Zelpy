CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lX11 -lxcb -Wl,-rpath-link,/usr/lib
PREFIX  = /usr/local

all: zelpy zelpane zelpyctl

zelpy: wm.c animation.c animation.h
	$(CC) $(CFLAGS) -o zelpy wm.c animation.c $(LDFLAGS)

zelpane: zelpane.c
	$(CC) $(CFLAGS) -o zelpane zelpane.c $(LDFLAGS)

zelpyctl: zelpyctl.c
	$(CC) $(CFLAGS) -o zelpyctl zelpyctl.c $(LDFLAGS)

install: all
	install -Dm755 zelpy    $(DESTDIR)$(PREFIX)/bin/zelpy
	install -Dm755 zelpane  $(DESTDIR)$(PREFIX)/bin/zelpane
	install -Dm755 zelpyctl $(DESTDIR)$(PREFIX)/bin/zelpyctl

clean:
	rm -f zelpy zelpane zelpyctl

.PHONY: all install clean
