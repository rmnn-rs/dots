PREFIX = /usr/local
CFLAGS = -std=gnu11 -Wall -Wextra -Os

cerberus-wl: cerberus.c config.h
	$(CC) $(CFLAGS) -o $@ cerberus.c $(LDFLAGS)

clean:
	rm -f cerberus-wl

install: cerberus-wl
	install -Dm755 cerberus-wl $(DESTDIR)$(PREFIX)/bin/cerberus-wl

.PHONY: clean install
