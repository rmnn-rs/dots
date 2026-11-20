CC      = gcc
CFLAGS  = -O3 -march=native -Wall -Werror
LIBS    = -lasound
PREFIX  = /usr/local
SRC     = volume.c
OBJ     = volume

# Desktop notifications need libnotify.  Build without them: make NOTIFY=0
NOTIFY ?= 1
ifeq ($(NOTIFY),1)
CFLAGS += -DHAVE_NOTIFY $(shell pkg-config --cflags libnotify)
LIBS   += $(shell pkg-config --libs libnotify)
endif

# Compile a different default sound card in:  make CARD=hw:0
ifdef CARD
CFLAGS += -DDEFAULT_CARD='"$(CARD)"'
endif

all: $(OBJ)

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(OBJ) $(LIBS)

install: all
	@echo "Installing to $(PREFIX)/bin"
	@install -Dm755 $(OBJ) $(PREFIX)/bin/$(OBJ)

uninstall:
	@echo "Removing from $(PREFIX)/bin"
	@rm -f $(PREFIX)/bin/$(OBJ)

clean:
	@echo "Cleaning build artifacts"
	@rm -f $(OBJ)

.PHONY: all install uninstall clean
