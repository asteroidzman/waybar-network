PLUGIN  := libnetwork.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 gtk-layer-shell-0
WBCOMMON ?= common
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC -I$(WBCOMMON) $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local/lib/waybar
DATADIR ?= $(HOME)/.local/share/waybar-network

$(PLUGIN): src/network.c $(WBCOMMON)/wbcommon.h
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(PREFIX)/$(PLUGIN)
	install -Dm644 -t $(DATADIR) assets/wifi1.svg assets/wifi2.svg assets/wifi3.svg assets/ethernet.svg assets/disconnected.svg
	@echo "installed to $(PREFIX)/$(PLUGIN) + icons in $(DATADIR)"

clean:
	rm -f $(PLUGIN)
.PHONY: install clean
