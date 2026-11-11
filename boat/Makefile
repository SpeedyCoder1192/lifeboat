CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -D_GNU_SOURCE
OBJS = main.o store.o probe.o home.o pkgs.o extras.o settings.o ram.o tools.o surface.o fix.o drivers.o wins.o toplevel-protocol.o ui.o

all: lifeboat lifeboat-splash

# the boot animation runs in the initramfs, so it has nothing but libc, linked in
lifeboat-splash: splash.c
	$(CC) -O2 -Wall -Wextra -static -o $@ splash.c -lm
	strip $@

lifeboat: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lsystemd -lalpm -lwayland-client -lm -lpthread
	strip $@

$(OBJS): boat.h probe.h ui.h pkgs.h extras.h wins.h toplevel-protocol.h

# the window list speaks wlr-foreign-toplevel-management, glue made from its xml
toplevel-protocol.h: toplevel.xml
	wayland-scanner client-header $< $@
toplevel-protocol.c: toplevel.xml
	wayland-scanner private-code $< $@

clean:
	rm -f $(OBJS) lifeboat lifeboat-splash toplevel-protocol.[ch]
