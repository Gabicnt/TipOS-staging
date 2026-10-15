#include <stdint.h>
#include <stddef.h>
#include "doomgeneric/doomgeneric.h"
#include "doomgeneric/doomkeys.h"

#define SYS_read 0
#define SYS_write 1
#define SYS_gettimeofday 116
#define SYS_kbhit 198
#define SYS_mouse_read 202
#define SYS_kbd_event 206
#define SYS_disp_get_fb 200
#define SYS_nanosleep 35

static long tipos_syscall4(long number, long first, long second, long third, long fourth) {
    long result;
    __asm__ volatile ("int $0x80"
        : "=a"(result)
        : "a"(number), "D"(first), "S"(second), "d"(third), "c"(fourth)
        : "r11", "memory");
    return result;
}

static uint32_t *framebuffer;
static uint32_t framebuffer_width;
static uint32_t framebuffer_height;
static uint32_t framebuffer_pitch;


void DG_Init(void) {
    uint64_t address = 0;
    tipos_syscall4(SYS_kbd_event, 1, 0, 0, 0);
    if (tipos_syscall4(SYS_disp_get_fb, (long)&address,
            (long)&framebuffer_width, (long)&framebuffer_height,
            (long)&framebuffer_pitch) == 0) {
        framebuffer = (uint32_t *)(uintptr_t)address;
    }
}

void DG_DrawFrame(void) {
    if (!framebuffer || !DG_ScreenBuffer) return;
    uint32_t scale_x = framebuffer_width / DOOMGENERIC_RESX;
    uint32_t scale_y = framebuffer_height / DOOMGENERIC_RESY;
    uint32_t scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale == 0) scale = 1;
    uint32_t draw_width = DOOMGENERIC_RESX * scale;
    uint32_t draw_height = DOOMGENERIC_RESY * scale;
    uint32_t left = (framebuffer_width - draw_width) / 2;
    uint32_t top = (framebuffer_height - draw_height) / 2;
    uint32_t pitch_pixels = framebuffer_pitch / sizeof(uint32_t);
    for (uint32_t y = 0; y < draw_height; y++) {
        uint32_t source_y = y / scale;
        uint32_t *destination = framebuffer + (top + y) * pitch_pixels + left;
        const uint32_t *source = DG_ScreenBuffer + source_y * DOOMGENERIC_RESX;
        for (uint32_t x = 0; x < draw_width; x++)
            destination[x] = source[x / scale];
    }
}

void DG_SleepMs(uint32_t milliseconds) {
    struct { uint64_t seconds; uint64_t nanoseconds; } request;
    request.seconds = milliseconds / 1000;
    request.nanoseconds = (uint64_t)(milliseconds % 1000) * 1000000ULL;
    tipos_syscall4(SYS_nanosleep, (long)&request, 0, 0, 0);
}

uint32_t DG_GetTicksMs(void) {
    struct { uint64_t seconds; uint64_t microseconds; } time;
    if (tipos_syscall4(SYS_gettimeofday, (long)&time, 0, 0, 0) != 0) return 0;
    return (uint32_t)(time.seconds * 1000 + time.microseconds / 1000);
}

/* ~~ Fila de eventos de teclado (press + release) ~~ */
#define KEYQUEUE_SIZE 32
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;
static int s_ExtendedKeyPrefix = 0;
static int s_PauseSequenceRemaining = 0;
static unsigned char s_KeyByScancode[256];
static int s_KeyTraceCount = 0;

void DG_TraceKey(char stage, unsigned char raw, unsigned char key, int pressed) {
    if ((key != KEY_UPARROW && key != KEY_DOWNARROW) || s_KeyTraceCount >= 96)
        return;
    char line[] = "[D?] 00 00 D\n";
    const char *hex = "0123456789ABCDEF";
    line[2] = stage;
    line[5] = hex[(raw >> 4) & 0xF];
    line[6] = hex[raw & 0xF];
    line[8] = hex[(key >> 4) & 0xF];
    line[9] = hex[key & 0xF];
    line[11] = pressed ? 'D' : 'U';
    tipos_syscall4(SYS_write, 2, (long)line, 13, 0);
    s_KeyTraceCount++;
}

static void push_key_event(unsigned short k, int is_press) {
    unsigned int next = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
    if (next == s_KeyQueueReadIndex) return;
    s_KeyQueue[s_KeyQueueWriteIndex] = k | (is_press ? 0 : 0x100);
    s_KeyQueueWriteIndex = next;
}

static unsigned char convert_scancode(unsigned char scan, int extended) {
    static const unsigned char normal_map[128] = {
        [0x01] = KEY_ESCAPE,
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
        [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
        [0x0A] = '9', [0x0B] = '0', [0x0C] = KEY_MINUS,
        [0x0D] = KEY_EQUALS, [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
        [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
        [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
        [0x1C] = KEY_ENTER, [0x1D] = KEY_FIRE,
        [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
        [0x21] = 'f', [0x22] = 'g',
        [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
        [0x27] = ';', [0x28] = '\'', [0x29] = '`',
        [0x2A] = KEY_RSHIFT, [0x2B] = '\\', [0x2C] = 'z',
        [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
        [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
        [0x35] = '/', [0x36] = KEY_RSHIFT, [0x37] = KEYP_MULTIPLY,
        [0x38] = KEY_RALT, [0x39] = KEY_USE, [0x3A] = KEY_CAPSLOCK,
        [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3,
        [0x3E] = KEY_F4, [0x3F] = KEY_F5, [0x40] = KEY_F6,
        [0x41] = KEY_F7, [0x42] = KEY_F8, [0x43] = KEY_F9,
        [0x44] = KEY_F10, [0x45] = KEY_NUMLOCK, [0x46] = KEY_SCRLCK,
        [0x47] = KEY_HOME, [0x48] = KEY_UPARROW, [0x49] = KEY_PGUP,
        [0x4A] = KEYP_MINUS, [0x4B] = KEY_LEFTARROW,
        [0x4C] = KEYP_5, [0x4D] = KEY_RIGHTARROW, [0x4E] = KEYP_PLUS,
        [0x4F] = KEY_END, [0x50] = KEY_DOWNARROW, [0x51] = KEY_PGDN,
        [0x52] = KEY_INS, [0x53] = KEY_DEL, [0x57] = KEY_F11,
        [0x56] = '\\', [0x58] = KEY_F12
    };

    if (scan >= 128) return 0;
    if (extended) {
        switch (scan) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1D: return KEY_FIRE;
        case 0x38: return KEY_RALT;
        case 0x1C: return KEYP_ENTER;
        case 0x35: return KEYP_DIVIDE;
        case 0x37: return KEY_PRTSCR;
        case 0x47: return KEY_HOME;
        case 0x49: return KEY_PGUP;
        case 0x4F: return KEY_END;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INS;
        case 0x53: return KEY_DEL;
        default: return 0;
        }
    }

    switch (scan) {
    case 0x11: return KEY_UPARROW;
    case 0x1E: return KEY_LEFTARROW;
    case 0x1F: return KEY_DOWNARROW;
    case 0x20: return KEY_RIGHTARROW;
    case 0x10: return KEY_STRAFE_L;
    case 0x12: return KEY_STRAFE_R;
    default: return normal_map[scan];
    }
}

static void pump_keys(void) {
    int count = 0;
    while (count < 16) {
        long input = tipos_syscall4(SYS_kbd_event, 0, 0, 0, 0);
        if (input >= 256) break;
        unsigned char raw = (unsigned char)input;
        if (s_PauseSequenceRemaining) {
            s_PauseSequenceRemaining--;
            count++;
            continue;
        }
        if (raw == 0xE1) {
            s_ExtendedKeyPrefix = 0;
            s_PauseSequenceRemaining = 5;
            count++;
            continue;
        }
        if (raw == 0xE0) {
            s_ExtendedKeyPrefix = 1;
            count++;
            continue;
        }
        int extended = s_ExtendedKeyPrefix;
        s_ExtendedKeyPrefix = 0;
        int is_release = (raw & 0x80) != 0;
        unsigned char scan = raw & 0x7F;
        unsigned int physical_key = scan + (extended ? 128 : 0);
        unsigned char dk;
        int was_down = s_KeyByScancode[physical_key] != 0;
        if (is_release) {
            dk = s_KeyByScancode[physical_key];
            s_KeyByScancode[physical_key] = 0;
            if (!dk) dk = convert_scancode(scan, extended);
        } else {
            dk = convert_scancode(scan, extended);
            s_KeyByScancode[physical_key] = dk;
        }
        if (dk && !(dk == KEY_RSHIFT && !is_release && was_down)) {
            DG_TraceKey('G', raw, dk, !is_release);
            push_key_event(dk, !is_release);
        }
        count++;
    }
}



int DG_GetKey(int *pressed, unsigned char *key) {
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) pump_keys();
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) return 0;
    unsigned short ev = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
    *pressed = (ev & 0x100) ? 0 : 1;
    *key = (unsigned char)(ev & 0xFF);
    DG_TraceKey('R', *key, *key, *pressed);
    return 1;
}

int DG_GetMouse(int *dx, int *dy, int *buttons) {
    return (int)tipos_syscall4(SYS_mouse_read, (long)dx, (long)dy,
            (long)buttons, 0);
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}