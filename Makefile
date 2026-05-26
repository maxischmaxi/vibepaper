CC      ?= cc
PKG      = wayland-client libcjson libcurl
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -Werror=implicit-function-declaration \
           -D_GNU_SOURCE -pthread \
           -Isrc -Iprotocols -Ithird_party \
           $(shell pkg-config --cflags $(PKG))
LDLIBS  := $(shell pkg-config --libs $(PKG)) -lm -lrt -pthread

WAYLAND_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null)
ifeq ($(WAYLAND_SCANNER),)
WAYLAND_SCANNER := wayland-scanner
endif

WP_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

PROTO_DIR  := protocols

# (xml, generated_basename) pairs.
LAYER_XML := $(PROTO_DIR)/wlr-layer-shell-unstable-v1.xml
XDG_XML   := $(WP_DIR)/stable/xdg-shell/xdg-shell.xml
VP_XML    := $(WP_DIR)/stable/viewporter/viewporter.xml

PROTO_HDRS := \
    $(PROTO_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h \
    $(PROTO_DIR)/xdg-shell-client-protocol.h \
    $(PROTO_DIR)/viewporter-client-protocol.h
PROTO_SRCS := \
    $(PROTO_DIR)/wlr-layer-shell-unstable-v1-protocol.c \
    $(PROTO_DIR)/xdg-shell-protocol.c \
    $(PROTO_DIR)/viewporter-protocol.c
PROTO_OBJS := $(PROTO_SRCS:.c=.o)

SRCS := src/main.c src/wayland.c src/image.c src/openai.c src/ipc.c src/daemon.c src/store.c
OBJS := $(SRCS:.c=.o) $(PROTO_OBJS)

BIN := background

.PHONY: all clean distclean run

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Source files depend on the generated protocol headers so the build order is correct.
src/%.o: src/%.c $(PROTO_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

protocols/%.o: protocols/%.c $(PROTO_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(PROTO_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_XML)
	$(WAYLAND_SCANNER) client-header $< $@
$(PROTO_DIR)/wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(PROTO_DIR)/xdg-shell-client-protocol.h: $(XDG_XML)
	$(WAYLAND_SCANNER) client-header $< $@
$(PROTO_DIR)/xdg-shell-protocol.c: $(XDG_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(PROTO_DIR)/viewporter-client-protocol.h: $(VP_XML)
	$(WAYLAND_SCANNER) client-header $< $@
$(PROTO_DIR)/viewporter-protocol.c: $(VP_XML)
	$(WAYLAND_SCANNER) private-code $< $@

clean:
	rm -f $(OBJS) $(BIN)

distclean: clean
	rm -f $(PROTO_HDRS) $(PROTO_SRCS)
