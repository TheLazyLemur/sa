CC ?= clang
PREFIX ?= /usr/local
CA_PEM ?= /etc/ssl/cert.pem

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS = -Wl,-dead_strip -Wl,-x
else
    LDFLAGS = -Wl,--gc-sections
endif

CFLAGS  = -Wall -Wextra -Os -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -ffunction-sections -fdata-sections \
          -Ibearssl/inc
LDLIBS  = bearssl/build/libbearssl.a

sa: main.c config.h ca.h bearssl/build/libbearssl.a
	$(CC) $(CFLAGS) $(LDFLAGS) main.c -o $@ $(LDLIBS)
	strip $@

config.h: config.def.h
	cp config.def.h $@

bearssl/build/libbearssl.a:
	cd bearssl && $(MAKE) CC=$(CC)

bearssl/build/brssl: bearssl/build/libbearssl.a

ca.h: bearssl/build/brssl
	./bearssl/build/brssl ta $(CA_PEM) > $@

install: sa
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 sa $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/sa

clean:
	rm -f sa
	cd bearssl && $(MAKE) clean

.PHONY: clean install uninstall
