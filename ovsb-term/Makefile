# ovsb.term — Makefile (moe moe kyun <3)
#
# Build host (Linux x86_64) do terminal Wayland nativo.
# Default = binário dinâmico p/ dev/teste. `make static` tenta estático
# total (blueprint original); `make install` joga no disk.img do TipOS.
#
# Desvios do blueprint (ARCHITECTURE.md Parte IX):
#  - loop usa poll() em vez de epoll (TipOS tem handler p/ poll; epoll é só mapa)
#  - signal.c/h renomeado p/ sigpipe.* (sombreava <signal.h> com -Isrc)
#  - xdg-shell gerado via wayland-scanner em src/generated/

CC      ?= gcc
STRIP   := strip
WL      := ../wl-build

CFLAGS  := -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra \
           -Isrc -Isrc/generated \
           -I$(WL)/wayland/include \
           -I$(WL)/fcft/include \
           -I$(WL)/pixman/include \
           -I$(WL)/xkbcommon/include \
           -I$(WL)/harfbuzz/include \
           -I/usr/include/freetype2 \
           -I/usr/include/libpng16 \
           -I/usr/include/fontconfig

LDFLAGS := -lpthread -lm -lrt -ldl

LIBS := $(WL)/fcft/libfcft.a \
        $(WL)/harfbuzz/libharfbuzz.a \
        $(WL)/pixman/libpixman-1.a \
        $(WL)/xkbcommon/libxkbcommon.a \
        $(WL)/wayland/libwayland-client.a \
        $(WL)/wayland/libwayland-util.a \
        $(WL)/libffi/libffi.a \
        /usr/lib/x86_64-linux-gnu/libfontconfig.a \
        /usr/lib/x86_64-linux-gnu/libfreetype.a \
        /usr/lib/x86_64-linux-gnu/libpng.a \
        /usr/lib/x86_64-linux-gnu/libutf8proc.a \
        -lz -lbz2 -lbrotlidec -lbrotlicommon -lexpat

SRCS := src/main.c src/config.c src/log.c src/util.c \
        src/wl_client.c src/loop.c \
        src/grid.c src/scrollback.c src/selection.c src/clipboard.c \
        src/escape.c src/input.c src/pty.c \
        src/font.c src/render.c src/glyph_cache.c src/cursor.c \
        src/shm.c src/timer.c src/sigpipe.c \
        src/generated/xdg-shell-protocol.c
OBJS := $(SRCS:.c=.o)

TARGET := ovsb.term

# Módulos sem deps externas (p/ testes unitários)
TEST_MODS := src/grid.c src/escape.c src/scrollback.c src/selection.c \
             src/clipboard.c src/pty.c src/util.c src/config.c src/log.c \
             src/sigpipe.c
TEST_BINS := tests/test_grid tests/test_escape tests/test_scrollback

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS) $(LDFLAGS)
	@ls -lh $@

src/generated/xdg-shell-client-protocol.h src/generated/xdg-shell-protocol.c: \
        /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
	mkdir -p src/generated
	wayland-scanner client-header $< src/generated/xdg-shell-client-protocol.h
	wayland-scanner private-code $< src/generated/xdg-shell-protocol.c

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ~~ estático total (blueprint) — best effort no host ~~
static: CFLAGS += -static -fno-stack-protector -mno-sse -mno-mmx -mgeneral-regs-only
static: LDFLAGS := -static -lpthread -lm -lrt -ldl
static: $(TARGET)
	$(STRIP) --strip-all $(TARGET) || true
	@ls -lh $(TARGET)

check: $(TEST_BINS)
	./tests/test_grid && ./tests/test_escape && ./tests/test_scrollback

tests/test_grid: tests/test_grid.c $(TEST_MODS)
	$(CC) -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Isrc -o $@ $^
tests/test_escape: tests/test_escape.c $(TEST_MODS)
	$(CC) -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Isrc -o $@ $^
tests/test_scrollback: tests/test_scrollback.c $(TEST_MODS)
	$(CC) -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Isrc -o $@ $^

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BINS)

install: $(TARGET)
	mcopy -o -i ../disk.img $(TARGET) ::/BIN/OVSB.TERM
	mkdir -p ../disk.img.d/usr/share/terminfo/o
	cp terminfo/ovsb.term ../disk.img.d/usr/share/terminfo/o/ovsb.term

.PHONY: all static check clean install
