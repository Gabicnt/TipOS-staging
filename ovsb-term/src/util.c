/* ovsb.term — util.c — cores, wcwidth, utf-8, base64 */
#include "term.h"

#include <string.h>
#include <stddef.h>

/* ~~ hex helper ~~ */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* compara case-insensitive (ascii) */
static int str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

uint32_t util_parse_color(const char *s)
{
    /* nomes básicos -> rgb (sem alpha; alpha FF aplicado no fim) */
    static const struct {
        const char *name;
        uint32_t rgb;
    } names[] = {
        { "black",         0x000000 },
        { "red",           0xCD3131 },
        { "green",         0x0DBC79 },
        { "yellow",        0xE5E510 },
        { "blue",          0x2472C8 },
        { "magenta",       0xBC3FBC },
        { "cyan",          0x11A8CD },
        { "white",         0xE5E5E5 },
        { "gray",          0x666666 },
        { "grey",          0x666666 },
        { "brightblack",   0x666666 },
        { "brightred",     0xF14C4C },
        { "brightgreen",   0x23D18B },
        { "brightyellow",  0xF5F543 },
        { "brightblue",    0x3B8EEA },
        { "brightmagenta", 0xD670D6 },
        { "brightcyan",    0x29B8DB },
        { "brightwhite",   0xFFFFFF },
    };
    size_t len, i;
    int h0, h1, h2, h3, h4, h5;
    unsigned r, g, b;

    if (s == NULL || *s == '\0')
        return 0xFFFFFFFFu;

    /* pula espaço inicial */
    while (*s == ' ' || *s == '\t')
        s++;

    if (s[0] == '#') {
        /* tira espaço/nl do fim */
        len = strlen(s);
        while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                           s[len - 1] == '\n' || s[len - 1] == '\r'))
            len--;
        if (len == 7) {
            h0 = hex_val(s[1]); h1 = hex_val(s[2]);
            h2 = hex_val(s[3]); h3 = hex_val(s[4]);
            h4 = hex_val(s[5]); h5 = hex_val(s[6]);
            if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0 || h5 < 0)
                return 0xFFFFFFFFu;
            r = (unsigned)((h0 << 4) | h1);
            g = (unsigned)((h2 << 4) | h3);
            b = (unsigned)((h4 << 4) | h5);
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        if (len == 4) {
            h0 = hex_val(s[1]); h1 = hex_val(s[2]); h2 = hex_val(s[3]);
            if (h0 < 0 || h1 < 0 || h2 < 0)
                return 0xFFFFFFFFu;
            r = (unsigned)((h0 << 4) | h0);
            g = (unsigned)((h1 << 4) | h1);
            b = (unsigned)((h2 << 4) | h2);
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        return 0xFFFFFFFFu;
    }

    /* nome: copia até espaço/; e compara */
    {
        char tmp[32];
        size_t n = 0;
        while (s[n] && s[n] != ' ' && s[n] != '\t' && s[n] != ';' && n < sizeof(tmp) - 1) {
            char c = s[n];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
            tmp[n] = c;
            n++;
        }
        tmp[n] = '\0';
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (str_eq_ci(tmp, names[i].name))
                return 0xFF000000u | names[i].rgb;
        }
    }
    return 0xFFFFFFFFu;
}

/* intervalos [lo,hi] */
typedef struct {
    uint32_t lo, hi;
} urange_t;

static int in_ranges(uint32_t cp, const urange_t *rs, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (cp >= rs[i].lo && cp <= rs[i].hi)
            return 1;
    }
    return 0;
}

int util_wcwidth(uint32_t cp)
{
    /* combining -> 0 */
    static const urange_t comb[] = {
        { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD },
        { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 }, { 0x05C4, 0x05C5 },
        { 0x05C7, 0x05C7 }, { 0x0610, 0x061A }, { 0x064B, 0x065F },
        { 0x0670, 0x0670 }, { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 },
        { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED }, { 0x0E31, 0x0E31 },
        { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E }, { 0x200B, 0x200F },
        { 0x20D0, 0x20FF }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
        { 0xE0100, 0xE01EF },
    };
    /* wide (CJK + emoji) -> 2 */
    static const urange_t wide[] = {
        { 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A },
        { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 },
        { 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 },
        { 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
        { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 },
        { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA },
        { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA },
        { 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B },
        { 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E },
        { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
        { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C },
        { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 }, { 0x2E80, 0x303E },
        { 0x3041, 0x3247 }, { 0x3250, 0x4DBF }, { 0x4E00, 0xA4C6 },
        { 0xA960, 0xA97C }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF },
        { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE4F }, { 0xFF00, 0xFF60 },
        { 0xFFE0, 0xFFE6 }, { 0x1F000, 0x1FAFF }, { 0x20000, 0x2FFFD },
        { 0x30000, 0x3FFFD },
    };

    if (cp == 0)
        return 0;
    if (in_ranges(cp, comb, sizeof(comb) / sizeof(comb[0])))
        return 0;
    if (in_ranges(cp, wide, sizeof(wide) / sizeof(wide[0])))
        return 2;
    return 1;
}

size_t util_utf8_encode(char *out, uint32_t cp)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        if (out)
            out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (out) {
            out[0] = (char)(0xC0 | (cp >> 6));
            out[1] = (char)(0x80 | (cp & 0x3F));
        }
        return 2;
    }
    if (cp < 0x10000) {
        if (out) {
            out[0] = (char)(0xE0 | (cp >> 12));
            out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (char)(0x80 | (cp & 0x3F));
        }
        return 3;
    }
    if (out) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
    }
    return 4;
}

int util_utf8_decode(const char *s, size_t n, uint32_t *cp)
{
    unsigned char c0, c1, c2, c3;
    uint32_t v;

    if (s == NULL || n == 0)
        return 0;
    c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        if (cp)
            *cp = c0;
        return 1;
    }
    if ((c0 & 0xE0) == 0xC0) {
        if (n < 2)
            return 0;
        c1 = (unsigned char)s[1];
        if ((c1 & 0xC0) != 0x80)
            goto invalid;
        v = ((uint32_t)(c0 & 0x1F) << 6) | (c1 & 0x3F);
        if (v < 0x80)
            goto invalid; /* overlong */
        if (cp)
            *cp = v;
        return 2;
    }
    if ((c0 & 0xF0) == 0xE0) {
        if (n < 3)
            return 0;
        c1 = (unsigned char)s[1];
        c2 = (unsigned char)s[2];
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
            goto invalid;
        v = ((uint32_t)(c0 & 0x0F) << 12) |
            ((uint32_t)(c1 & 0x3F) << 6) | (c2 & 0x3F);
        if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF))
            goto invalid;
        if (cp)
            *cp = v;
        return 3;
    }
    if ((c0 & 0xF8) == 0xF0) {
        if (n < 4)
            return 0;
        c1 = (unsigned char)s[1];
        c2 = (unsigned char)s[2];
        c3 = (unsigned char)s[3];
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 ||
            (c3 & 0xC0) != 0x80)
            goto invalid;
        v = ((uint32_t)(c0 & 0x07) << 18) |
            ((uint32_t)(c1 & 0x3F) << 12) |
            ((uint32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF)
            goto invalid;
        if (cp)
            *cp = v;
        return 4;
    }
invalid:
    if (cp)
        *cp = 0xFFFD;
    return 1;
}

void util_base64_encode(const char *in, size_t n, char *out, size_t outsz)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;

    if (out == NULL || outsz == 0)
        return;
    if (in == NULL || n == 0) {
        out[0] = '\0';
        return;
    }
    while (i < n) {
        unsigned char a = (unsigned char)in[i++];
        unsigned char b = 0, c = 0;
        int have_b = 0, have_c = 0;
        char o0, o1, o2, o3;

        if (i < n) {
            b = (unsigned char)in[i++];
            have_b = 1;
        }
        if (i < n) {
            c = (unsigned char)in[i++];
            have_c = 1;
        }
        o0 = tbl[a >> 2];
        o1 = tbl[((a & 0x03) << 4) | (have_b ? (b >> 4) : 0)];
        o2 = have_b ? tbl[((b & 0x0F) << 2) | (have_c ? (c >> 6) : 0)] : '=';
        o3 = have_c ? tbl[c & 0x3F] : '=';
        if (o + 4 >= outsz)
            break; /* sem espaço (guarda NUL) */
        out[o++] = o0;
        out[o++] = o1;
        out[o++] = o2;
        out[o++] = o3;
    }
    out[o] = '\0';
}
