CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
PREFIX ?= /usr/local
BIN = docker-event-listener

all: $(BIN)

$(BIN): docker-event-listener.c
	$(CC) $(CFLAGS) $< -o $@

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	ln -sf $(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/bin/gluetun-event-listener

clean:
	rm -f $(BIN) gluetun-event-listener *.o

.PHONY: all install clean
