CC ?= clang
PREFIX ?= /usr/local

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS = -Wl,-dead_strip -Wl,-x
else
    LDFLAGS = -Wl,--gc-sections
endif

CFLAGS  = -Wall -Wextra -Os -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -ffunction-sections -fdata-sections \
          $(shell curl-config --cflags)
LDLIBS  = $(shell curl-config --libs)

tiny_c: main.c config.h
	$(CC) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)
	strip $@

config.h: config.def.h
	cp config.def.h $@

install: tiny_c
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 tiny_c $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tiny_c

clean:
	rm -f tiny_c

.PHONY: clean install uninstall
