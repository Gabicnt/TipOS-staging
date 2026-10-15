# moe moe kyun <3
# ♥ Makefile OvsbMkM ~ Kernel OvsbMk para TipOS
# Compila C, NASM, e Zig pra gerar kernel.elf~
# Depois empacota em ISO com GRUB~ tudo automático! kyun!

# ♥ Toolchain: GCC pra C, NASM pra assembly, Zig pros drivers modernos~
CC := gcc
NASM := nasm
ZIG ?= zig
ifeq (,$(shell command -v $(ZIG) 2>/dev/null))
ZIG := /home/vinberkuko/.local/zig-x86_64-linux-0.16.0/zig
endif
# ♥ CFLAGS: freestanding (sem libc), no-red-zone, sem SSE/MMX~
# Só registradores de propósito geral ~ modo 64 bits puro!
CFLAGS := -ffreestanding -nostdlib -mno-red-zone -mno-mmx -mno-sse
CFLAGS += -mgeneral-regs-only -m64 -O2 -Wall
# ♥ ZIGFLAGS: target freestanding, soft-float só, sem SIMD~
ZIGFLAGS := -target x86_64-freestanding -mno-red-zone -O ReleaseSmall
ZIGFLAGS += -mcpu x86_64+soft_float-mmx-sse-sse2-sse3-ssse3-sse4_1-sse4_2-avx-avx2
# ♥ LDFLAGS: link cru, sem libc, sem PIE, linker script customizado
LDFLAGS := -nostdlib -no-pie -Wl,-nostdlib -T kernel/linker.ld

# ♥ BUILD_DIR: tudo vai pra build/ (mantém src limpinho)
BUILD_DIR := build

# ♥ VPATH: procura sources em kernel, drivers, libs, e fs~
VPATH := kernel:kernel/drivers:drivers:lib/gui:lib/owt:lib/wm:fs

# ♥ C_SRCS: todos .c (keyboard/mouse em Zig, não em C)
C_SRCS := $(notdir $(wildcard kernel/*.c kernel/drivers/*.c drivers/*.c lib/gui/*.c lib/owt/*.c lib/wm/*.c fs/*.c))
# ♥ ASM_SRCS: boot, IDT assembly, keyboard asm, context switch, syscall entry
ASM_SRCS := boot64.o idt_asm.o keyboard_asm.o switch_asm.o syscall_entry.o
# ♥ ZIG_SRCS: syscall handler, ELF loader, ext2, initramfs
ZIG_SRCS := syscall_linux.o elf64.o ext2.o ext2_new.o initramfs.o
# ♥ ZIG_DRV_SRCS: keyboard e mouse em Zig (moderno~)
ZIG_DRV_SRCS := keyboard.o mouse.o
# ♥ OBJS: todos os .o concatenados (C + ASM + Zig)
OBJS := $(addprefix $(BUILD_DIR)/,$(C_SRCS:.c=.o) $(ASM_SRCS) $(ZIG_SRCS) $(ZIG_DRV_SRCS))

# ♥ all: target padrão ~ compila kernel.elf
all: $(BUILD_DIR)/kernel.elf

# ♥ Cria diretório build/ se não existir
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ♥ Monta boot64.asm (entry point do kernel ~ multiboot header)
$(BUILD_DIR)/boot64.o: boot64.asm | $(BUILD_DIR)
	nasm -f elf64 -o $@ $<

# ♥ Monta idt.asm (save/restore de regs + iretq)
$(BUILD_DIR)/idt_asm.o: idt.asm | $(BUILD_DIR)
	nasm -f elf64 -o $@ $<

# ♥ Monta keyboard_asm.asm (ISR do teclado em asm)
$(BUILD_DIR)/keyboard_asm.o: keyboard_asm.asm | $(BUILD_DIR)
	nasm -f elf64 -o $@ $<

# ♥ Monta switch.asm (context_switch ~ salva/restaura regs + CR3)
$(BUILD_DIR)/switch_asm.o: switch.asm | $(BUILD_DIR)
	nasm -f elf64 -o $@ $<

# ♥ Monta syscall_entry.asm (syscall instruction handler ~ MSR LSTAR)
$(BUILD_DIR)/syscall_entry.o: kernel/syscall_entry.asm | $(BUILD_DIR)
	nasm -f elf64 -o $@ $<

# ♥ Compila syscall_linux.zig (handler de syscall via int 0x80)
$(BUILD_DIR)/syscall_linux.o: kernel/syscall_linux.zig | $(BUILD_DIR)
	$(ZIG) build-obj $(ZIGFLAGS) -femit-bin=$@ $<

# ♥ Compila elf64.zig (loader de ELF64)
$(BUILD_DIR)/elf64.o: kernel/elf64.zig | $(BUILD_DIR)
	$(ZIG) build-obj $(ZIGFLAGS) -femit-bin=$@ $<

# ♥ Compila keyboard.zig (driver de teclado PS/2 em Zig)
$(BUILD_DIR)/keyboard.o: kernel/drivers/keyboard.zig | $(BUILD_DIR)
	$(ZIG) build-obj $(ZIGFLAGS) -femit-bin=$@ $<

# ♥ Compila mouse.zig (driver de mouse PS/2 em Zig)
$(BUILD_DIR)/mouse.o: kernel/drivers/mouse.zig | $(BUILD_DIR)
	$(ZIG) build-obj $(ZIGFLAGS) -femit-bin=$@ $<

# ♥ Compila ext2.zig (sistema de arquivos ext2)
$(BUILD_DIR)/ext2.o: fs/ext2.zig | $(BUILD_DIR)
	$(ZIG) build-obj $(ZIGFLAGS) -femit-bin=$@ $<

$(BUILD_DIR)/ext2_new.o: fs/ext2_new.zig | $(BUILD_DIR)
	$(ZIG) build-obj $(ZIGFLAGS) -femit-bin=$@ $<

# ♥ Compila initramfs.zig (initramfs ~ arquivos iniciais)
$(BUILD_DIR)/initramfs.o: fs/initramfs.zig | $(BUILD_DIR)
	$(ZIG) build-obj $(ZIGFLAGS) -femit-bin=$@ $<

# ♥ Regra genérica: compila .c → .o (com includes nos diretórios)
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I. -Ikernel -Ikernel/drivers -Idrivers -Ilib/gui -Ilib/owt -Ilib/wm -Ifs -c -o $@ $<

# ♥ Link final: junta todos os .o no kernel.elf
$(BUILD_DIR)/kernel.elf: $(OBJS) kernel/linker.ld
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

# ♥ iso: gera ISO bootável com GRUB~
iso: OvsbMkM.iso
	@echo "OvsbMkM.iso gerada!"

# ♥ Tests: ELF test binaries compilados com musl-gcc
TESTS_DIR := tests
BUILD_TESTS := /tmp/HELLO /tmp/TTEST /tmp/TTEST50

$(BUILD_TESTS): $(TESTS_DIR)/hello.c $(TESTS_DIR)/ttest.c $(TESTS_DIR)/ttest50.c
	@echo "  Compilando HELLO..."
	@test -f /tmp/musl-gcc-custom && \
		/tmp/musl-gcc-custom -static -O2 -nostartfiles -fno-stack-protector \
			-nostdlib -ffreestanding -e _start $(TESTS_DIR)/hello.c -o /tmp/HELLO \
			-Wl,--build-id=none 2>/dev/null || true
	@echo "  Compilando TTEST..."
	@test -f /tmp/musl-gcc-custom && \
		/tmp/musl-gcc-custom -static -O2 -nostartfiles -fno-stack-protector \
			-nostdlib -ffreestanding -e _start $(TESTS_DIR)/ttest.c -o /tmp/TTEST \
			-Wl,--build-id=none 2>/dev/null || true
	@echo "  Compilando TTEST50..."
	@test -f /tmp/musl-gcc-custom && \
		/tmp/musl-gcc-custom -static -O2 -nostartfiles -fno-stack-protector \
			-nostdlib -ffreestanding -e _start $(TESTS_DIR)/ttest50.c -o /tmp/TTEST50 \
			-Wl,--build-id=none 2>/dev/null || true

# ♥ disk.img: disco virtual de 64MB com FAT32 (pra testes)
disk.img: $(BUILD_TESTS)
	dd if=/dev/zero of=$@ bs=1M count=64
	/sbin/mkfs.fat -F 32 $@
	@echo "disk.img criado!"
	@echo "  Instalando HELLO..."
	@mcopy -o -i $@ /tmp/HELLO ::/HELLO 2>/dev/null && echo "    OK" || echo "    FAIL"
	@echo "  Instalando TTEST..."
	@mcopy -o -i $@ /tmp/TTEST ::/TTEST 2>/dev/null && echo "    OK" || echo "    FAIL"
	@echo "  Instalando TTEST50..."
	@mcopy -o -i $@ /tmp/TTEST50 ::/TTEST50 2>/dev/null && echo "    OK" || echo "    FAIL"

# ♥ clean: limpa tudo (build/, isos, bins)
clean:
	rm -rf $(BUILD_DIR) *.o kernel.elf OvsbMkM.iso
	rm -rf iso/boot/kernel.elf

# ♥ run: gera ISO + disco, sobe QEMU com VGA virtio, 512MB RAM, serial, KVM
run: OvsbMkM.iso disk.img
	qemu-system-x86_64 -vga virtio -boot d -cdrom OvsbMkM.iso -m 512M -serial stdio -enable-kvm -drive file=disk.img,format=raw,if=ide

# ♥ run-no-kvm: mesma coisa mas sem KVM (WSL2 / sem aceleração)
run-no-kvm: OvsbMkM.iso disk.img
	qemu-system-x86_64 -vga virtio -boot d -cdrom OvsbMkM.iso -m 512M -serial stdio -drive file=disk.img,format=raw,if=ide

# ♥ Gera ISO com GRUB: copia kernel pra iso/boot e roda grub-mkrescue
OvsbMkM.iso: $(BUILD_DIR)/kernel.elf
	mkdir -p iso/boot
	cp $(BUILD_DIR)/kernel.elf iso/boot/
	grub-mkrescue -o OvsbMkM.iso iso 2>/dev/null || true

$(BUILD_DIR)/PTY_TEST: $(TESTS_DIR)/pty_test.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -Wextra -o $@ $<

build-pty-test: $(BUILD_DIR)/PTY_TEST
	@test -f ../disk.img
	@mcopy -o -i ../disk.img $(BUILD_DIR)/PTY_TEST ::/BIN/PTY_TEST

$(BUILD_DIR)/SIGNAL_TEST: $(TESTS_DIR)/signal_test.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

$(BUILD_DIR)/SIGCHLD_TEST: $(TESTS_DIR)/sigchld_test.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

$(BUILD_DIR)/SIGWINCH_TEST: $(TESTS_DIR)/sigwinch_test.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

build-signal-test: $(BUILD_DIR)/SIGNAL_TEST
	mcopy -o -i ../disk.img $(BUILD_DIR)/SIGNAL_TEST ::/BIN/SIGNAL_TEST

build-sigchld-test: $(BUILD_DIR)/SIGCHLD_TEST
	mcopy -o -i ../disk.img $(BUILD_DIR)/SIGCHLD_TEST ::/BIN/SIGCHLD_TEST

build-sigwinch-test: $(BUILD_DIR)/SIGWINCH_TEST
	mcopy -o -i ../disk.img $(BUILD_DIR)/SIGWINCH_TEST ::/BIN/SIGWINCH_TEST

build-signal-tests: build-signal-test build-sigchld-test build-sigwinch-test

# ♥ FASE 4: ELF dinâmico (libtest.so + DYN_TEST)
$(BUILD_DIR)/libtest.so: $(TESTS_DIR)/libtest.c | $(BUILD_DIR)
	gcc -shared -fPIC -nostdlib -Wl,--hash-style=sysv -o $@ $<

$(BUILD_DIR)/DYN_TEST: $(TESTS_DIR)/dyn_test.c $(BUILD_DIR)/libtest.so | $(BUILD_DIR)
	gcc -pie -nostdlib -fPIC -Wl,--hash-style=sysv -Wl,-rpath,/lib \
		-Wl,--dynamic-linker=/lib/ld.so -o $@ $< -L$(BUILD_DIR) -ltest

build-dyn-test: $(BUILD_DIR)/DYN_TEST $(BUILD_DIR)/libtest.so
	-mmd -i ../disk.img ::/LIB 2>/dev/null || true
	mcopy -o -i ../disk.img $(BUILD_DIR)/libtest.so ::/LIB/libtest.so
	mcopy -o -i ../disk.img $(BUILD_DIR)/DYN_TEST ::/BIN/DYN_TEST

.PHONY: all clean run iso build-pty-test build-signal-test build-sigchld-test build-sigwinch-test build-signal-tests build-dyn-test build-wayland-lib build-layer-shell-test build-ovsb-desktop
$(BUILD_DIR)/FB_EVDEV_TEST: $(TESTS_DIR)/fb_evdev_test.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

build-fb-evdev-test: $(BUILD_DIR)/FB_EVDEV_TEST
	mcopy -o -i ../disk.img $(BUILD_DIR)/FB_EVDEV_TEST ::/BIN/FB_EVDEV_TEST

$(BUILD_DIR)/WAYLAND_COMPOSITOR: $(TESTS_DIR)/wayland_compositor.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

$(BUILD_DIR)/WAYLAND_CLIENT: $(TESTS_DIR)/wayland_client.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

$(BUILD_DIR)/WAYLAND_COMPOSITOR_SHM: $(TESTS_DIR)/wayland_compositor_shm.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

$(BUILD_DIR)/WAYLAND_SHM_CLIENT: $(TESTS_DIR)/wayland_shm_client.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

build-wayland: $(BUILD_DIR)/WAYLAND_COMPOSITOR $(BUILD_DIR)/WAYLAND_CLIENT $(BUILD_DIR)/WAYLAND_COMPOSITOR_SHM $(BUILD_DIR)/WAYLAND_SHM_CLIENT
	mcopy -o -i ../disk.img $(BUILD_DIR)/WAYLAND_COMPOSITOR ::/BIN/WAYLAND_COMPOSITOR
	mcopy -o -i ../disk.img $(BUILD_DIR)/WAYLAND_CLIENT ::/BIN/WAYLAND_CLIENT
	mcopy -o -i ../disk.img $(BUILD_DIR)/WAYLAND_COMPOSITOR_SHM ::/BIN/WAYLAND_COMPOSITOR_SHM
	mcopy -o -i ../disk.img $(BUILD_DIR)/WAYLAND_SHM_CLIENT ::/BIN/WAYLAND_SHM_CLIENT

# ♥ FASE 7: compositor lib (libwayland-server + xdg_shell, static).
# Receita reconstruída na Fase 7 (antes era comando manual perdido).
# WL_DIR aponta p/ ../wl-build (libs estáticas sem SIMD).
WL_DIR := ../wl-build
$(BUILD_DIR)/WAYLAND_COMPOSITOR_LIB: $(TESTS_DIR)/compositor/wayland_compositor_lib.c $(TESTS_DIR)/compositor/xdg-shell-protocol.c $(TESTS_DIR)/compositor/us_keymap_bin.c | $(BUILD_DIR)
	$(CC) -static -g -O2 -Wall -I $(TESTS_DIR)/compositor -I $(WL_DIR)/wayland/include -o $@ \
		$(TESTS_DIR)/compositor/wayland_compositor_lib.c $(TESTS_DIR)/compositor/xdg-shell-protocol.c \
		$(WL_DIR)/wayland/libwayland-server.a $(WL_DIR)/wayland/libwayland-private.a \
		$(WL_DIR)/wayland/libwayland-util.a $(WL_DIR)/libffi/libffi.a \
		-lrt -lpthread -ldl -lm

build-wayland-lib: $(BUILD_DIR)/WAYLAND_COMPOSITOR_LIB
	mcopy -o -i ../disk.img $(BUILD_DIR)/WAYLAND_COMPOSITOR_LIB ::/BIN/WAYLAND_COMPOSITOR_LIB

# ♥ FASE 8.0a: teste do wlr-layer-shell (wallpaper + barra, protocolo raw)
$(BUILD_DIR)/LAYER_SHELL_TEST: $(TESTS_DIR)/layer_shell_test.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

build-layer-shell-test: $(BUILD_DIR)/LAYER_SHELL_TEST
	mcopy -o -i ../disk.img $(BUILD_DIR)/LAYER_SHELL_TEST ::/BIN/LAYER_SHELL_TEST

# ♥ FASE 8.1: ovsb-desktop (wallpaper + barra + relógio, protocolo raw)
$(BUILD_DIR)/OVSB-DESKTOP: $(TESTS_DIR)/desktop/ovsb-desktop.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -o $@ $<

build-ovsb-desktop: $(BUILD_DIR)/OVSB-DESKTOP
	mcopy -o -i ../disk.img $(BUILD_DIR)/OVSB-DESKTOP ::/BIN/OVSB-DESKTOP

$(BUILD_DIR)/MEMFD_TEST: $(TESTS_DIR)/memfd_test.c | $(BUILD_DIR)
	gcc -static -O2 -Wall -fno-stack-protector -U_FORTIFY_SOURCE -o $@ $<

build-memfd-test: $(BUILD_DIR)/MEMFD_TEST 
$(BUILD_DIR)/MEMFD_SIMPLE: $(TESTS_DIR)/memfd_simple.c | $(BUILD_DIR)
	$(CC) -static -O2 -Wall -fno-stack-protector -U_FORTIFY_SOURCE -o $@ $<

build-memfd-simple: $(BUILD_DIR)/MEMFD_SIMPLE
	mcopy -o -i ../disk.img $(BUILD_DIR)/MEMFD_SIMPLE ::/BIN/MEMFD_SIMPLE
	mcopy -o -i ../disk.img $(BUILD_DIR)/MEMFD_TEST ::/BIN/MEMFD_TEST

