CFLAGS ?= -O2 -Wall -Wextra -I/opt/homebrew/include/libusb-1.0
awp_rndisd: awp_rndisd.c
	cc $(CFLAGS) -o $@ $< /opt/homebrew/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security -lobjc -lpthread
clean:
	rm -f awp_rndisd
