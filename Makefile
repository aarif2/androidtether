PREFIX ?= /usr/local
LIBUSB_PREFIX ?= $(shell brew --prefix libusb 2>/dev/null || echo /opt/homebrew)
CFLAGS ?= -O2 -Wall -Wextra

androidtetherd: awp_rndisd.c
	cc $(CFLAGS) -I$(LIBUSB_PREFIX)/include/libusb-1.0 -o $@ $< $(LIBUSB_PREFIX)/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security -lobjc -lpthread

install: androidtetherd
	install -d $(PREFIX)/sbin
	install -m 755 androidtetherd $(PREFIX)/sbin/androidtetherd

clean:
	rm -f androidtetherd

.PHONY: install clean
