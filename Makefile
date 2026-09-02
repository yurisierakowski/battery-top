CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lncurses
TARGET  := battery-top

.PHONY: all clean install

all: $(TARGET)

$(TARGET): battery-top.c
	$(CC) $(CFLAGS) -o $(TARGET) battery-top.c $(LDLIBS)

clean:
	rm -f $(TARGET)

# Installs to /usr/local/bin by default. Override with `make install PREFIX=~/.local`.
# DESTDIR is a staging root prepended to PREFIX (e.g. packaging builds);
# leave it unset for a normal install straight onto the running system.
PREFIX ?= /usr/local
install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
