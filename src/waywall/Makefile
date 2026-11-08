CC = gcc
PKGS = imlib2 wayland-client
CFLAGS = -fPIE -pie -Wl,-z,relro,-z,now -fstack-protector-strong -march=native -flto -O3 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LIBS = $(shell pkg-config --libs $(PKGS)) -lsqlite3

# Protocol XMLs: layer-shell is vendored in this directory (same protocol on wlroots and
# neuswc, no wlr-protocols package needed); xdg-shell comes from wayland-protocols
WLR_XML = wlr-layer-shell-unstable-v1.xml
XDG_XML ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

GEN_H = wlr-layer-shell-unstable-v1-client-protocol.h
GEN_C = wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c

TARGET = waywall
SRC = wallman.c wall.c walldb.c wallwatch.c $(GEN_C)
OBJ = $(SRC:.c=.o)

all: $(TARGET)

# Generated protocol glue (xdg-shell is only needed for the xdg_popup symbol layer-shell references)
$(GEN_H): $(WLR_XML)
	wayland-scanner client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: $(WLR_XML)
	wayland-scanner private-code $< $@

xdg-shell-protocol.c: $(XDG_XML)
	wayland-scanner private-code $< $@

wall.o: $(GEN_H)

# Generic rule to compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link the target - CRITICAL: $(LIBS) must be at the end
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) $(LIBS)

clean:
	rm -f $(TARGET) $(OBJ) $(GEN_H) $(GEN_C)

install: all
	mkdir -p /usr/local/bin
	cp -f $(TARGET) /usr/local/bin
	chmod 755 /usr/local/bin/$(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
