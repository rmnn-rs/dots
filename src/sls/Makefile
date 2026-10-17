# See LICENSE file for copyright and license details
# slstatus - suckless status monitor
.POSIX:

include config.mk

CC = cc

REQ = util
COM =\
	components/battery\
	components/cat\
	components/cpu\
	components/datetime\
	components/disk\
	components/entropy\
	components/hostname\
	components/ip\
	components/kernel_release\
	components/keyboard_indicators\
	components/keymap\
	components/load_avg\
	components/netspeeds\
	components/num_files\
	components/ram\
	components/run_command\
	components/swap\
	components/temperature\
	components/uptime\
	components/user\
	components/volume\
	components/wifi\
	components/bri\
	components/music\
	components/wall

all: sls

$(COM:=.o): config.mk $(REQ:=.h) slstatus.h
slstatus.o: slstatus.c slstatus.h arg.h config.h config.mk $(REQ:=.h)

.c.o:
	$(CC) -o $@ -c $(CPPFLAGS) $(CFLAGS) $<

sls: slstatus.o $(COM:=.o) $(REQ:=.o)
	$(CC) -o $@ $(LDFLAGS) $(COM:=.o) $(REQ:=.o) slstatus.o $(LDLIBS)

clean:
	rm -f sls slstatus.o $(COM:=.o) $(REQ:=.o) sls-${VERSION}.tar.gz

dist:
	rm -rf "sls-$(VERSION)"
	mkdir -p "sls-$(VERSION)/components"
	cp -R LICENSE Makefile README config.mk config.def.h \
	      arg.h slstatus.h slstatus.c $(REQ:=.c) $(REQ:=.h) \
	      slstatus.1 "sls-$(VERSION)"
	cp -R $(COM:=.c) "sls-$(VERSION)/components"
	tar -cf - "sls-$(VERSION)" | gzip -c > "slstatus-$(VERSION).tar.gz"
	rm -rf "sls-$(VERSION)"

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	cp -f sls "$(DESTDIR)$(PREFIX)/bin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/sls"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/sls"
