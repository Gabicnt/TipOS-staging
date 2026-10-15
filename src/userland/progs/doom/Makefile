CC ?= gcc
USERLAND := ../..
GCC_INCLUDE := $(shell $(CC) -print-file-name=include)
DOOM_DIR := doomgeneric

EXCLUDED := doomgeneric_xlib.c doomgeneric_sdl.c doomgeneric_win.c \
            doomgeneric_allegro.c doomgeneric_emscripten.c doomgeneric_linuxvt.c \
            doomgeneric_soso.c doomgeneric_sosox.c i_sdlmusic.c i_sdlsound.c \
            i_allegromusic.c i_allegrosound.c i_cdmus.c i_joystick.c sha1.c \
            statdump.c dummy.c i_endoom.c
DOOM_SOURCES := $(filter-out $(addprefix $(DOOM_DIR)/,$(EXCLUDED)),$(wildcard $(DOOM_DIR)/*.c))
LIBC_SOURCES := $(USERLAND)/libc/crt0.c $(USERLAND)/libc/stdio.c \
                $(USERLAND)/libc/stdlib.c $(USERLAND)/libc/string.c \
                $(USERLAND)/libc/tui.c

CFLAGS := -ffreestanding -nostdlib -nostdinc -static -fno-PIC -mno-red-zone \
          -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-unwind-tables -include stddef.h -O1 \
          -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
          -isystem $(USERLAND)/include -isystem $(GCC_INCLUDE) -I$(DOOM_DIR)
LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none -T ../../libc/link.ld -e _start

.PHONY: all clean fbtest
all: DOOM.ELF

fbtest: FBTEST.ELF

FBTEST.ELF: fbtest.c $(USERLAND)/libc/crt0.c $(USERLAND)/libc/stdlib.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ fbtest.c $(USERLAND)/libc/crt0.c $(USERLAND)/libc/stdlib.c

DOOM.ELF: $(DOOM_SOURCES) tipOS_glue.c tipOS_doom.c tipOS_stubs.c $(LIBC_SOURCES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tipOS_doom.c tipOS_glue.c tipOS_stubs.c $(DOOM_SOURCES) $(LIBC_SOURCES)

clean:
	rm -f DOOM.ELF FBTEST.ELF