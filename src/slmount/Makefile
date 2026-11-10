CC = cc
CFLAGS = -std=c99 -pedantic -Wall -Werror -Os
LDFLAGS = -ludev -lcryptsetup -lblkid -lncurses
PREFIX = /usr/local

all: slmount

slmount: slmount.c config.h
	$(CC) $(CFLAGS) slmount.c -o slmount $(LDFLAGS)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f slmount $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/slmount

clean:
	rm -f slmount

.PHONY: all install clean
