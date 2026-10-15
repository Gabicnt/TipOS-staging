#include "doomgeneric/doomtype.h"
#include "doomgeneric/i_joystick.h"
#include "doomgeneric/i_endoom.h"
#include "doomgeneric/d_player.h"
#include "doomgeneric/statdump.h"
#include "doomgeneric/net_client.h"
#include "doomgeneric/sha1.h"

double fabs(double value) {
    return value < 0.0 ? -value : value;
}

double atof(const char *text) {
    double value = 0.0;
    double fraction = 0.1;
    int sign = 1;
    while (*text == ' ' || *text == '\t') text++;
    if (*text == '-') { sign = -1; text++; }
    else if (*text == '+') text++;
    while (*text >= '0' && *text <= '9') value = value * 10.0 + (*text++ - '0');
    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9') {
            value += (*text++ - '0') * fraction;
            fraction *= 0.1;
        }
    }
    return sign * value;
}

boolean net_client_connected = false;
boolean drone = false;

void I_InitJoystick(void) {}
void I_ShutdownJoystick(void) {}
void I_UpdateJoystick(void) {}
void I_BindJoystickVariables(void) {}
void I_Endoom(byte *data) { (void)data; }
void StatCopy(wbstartstruct_t *stats) { (void)stats; }
void StatDump(void) {}

void SHA1_Init(sha1_context_t *context) {
    byte *bytes = (byte *)context;
    for (unsigned long i = 0; i < sizeof(*context); i++) bytes[i] = 0;
}

void SHA1_Update(sha1_context_t *context, byte *buffer, size_t length) {
    (void)context;
    (void)buffer;
    (void)length;
}

void SHA1_UpdateInt32(sha1_context_t *context, unsigned int value) {
    (void)context;
    (void)value;
}

void SHA1_UpdateString(sha1_context_t *context, char *value) {
    (void)context;
    (void)value;
}

void SHA1_Final(sha1_digest_t digest, sha1_context_t *context) {
    (void)context;
    for (unsigned int i = 0; i < sizeof(sha1_digest_t); i++) digest[i] = 0;
}