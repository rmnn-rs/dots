PREFIX = /usr/local
CFLAGS = -std=gnu11 -Wall -Wextra -Os -Werror

cerberus: cerberus.c config.h
	$(CC) $(CFLAGS) -o $@ cerberus.c $(LDFLAGS)

clean:
	rm -f cerberus

install: cerberus
	install -Dm755 cerberus $(DESTDIR)$(PREFIX)/bin/cerberus

.PHONY: clean install
