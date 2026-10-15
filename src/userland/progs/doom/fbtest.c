#include <stdint.h>

#define SYS_exit 60
#define SYS_nanosleep 35
#define SYS_disp_get_fb 200

static long syscall4(long number, long first, long second, long third, long fourth) {
    long result;
    __asm__ volatile ("int $0x80"
        : "=a"(result)
        : "a"(number), "D"(first), "S"(second), "d"(third), "c"(fourth)
        : "r11", "memory");
    return result;
}

int main(void) {
    uint64_t address = 0;
    uint32_t width = 0, height = 0, pitch = 0;
    if (syscall4(SYS_disp_get_fb, (long)&address, (long)&width,
                 (long)&height, (long)&pitch) != 0 || address == 0)
        return 1;

    uint32_t *framebuffer = (uint32_t *)(uintptr_t)address;
    uint32_t pitch_pixels = pitch / sizeof(uint32_t);
    const uint32_t colors[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF };
    for (uint32_t y = 0; y < height; y++) {
        uint32_t color = colors[((uint64_t)y * 4) / height];
        for (uint32_t x = 0; x < width; x++)
            framebuffer[y * pitch_pixels + x] = color;
    }

    struct { uint64_t seconds; uint64_t nanoseconds; } delay = { 1, 0 };
    for (;;) syscall4(SYS_nanosleep, (long)&delay, 0, 0, 0);
}
