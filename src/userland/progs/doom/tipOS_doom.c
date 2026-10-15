#include "doomgeneric/doomgeneric.h"

extern void DG_SleepMs(unsigned int milliseconds);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char *doom_argv[] = { "doom", "-iwad", "/APPS/DOOM1.WAD", 0 };
    doomgeneric_Create(3, doom_argv);
    for (;;) {
        doomgeneric_Tick();
        DG_SleepMs(10);
    }
    return 0;
}