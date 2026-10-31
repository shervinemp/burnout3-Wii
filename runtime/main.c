/*
 * Runtime entry: load the executable and start the EE kernel.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rt.h"
#include "hw.h"

#include <string.h>

extern int g_dump_every;

int rt_main(int argc, char **argv)
{
    uint32_t entry;
    int i;
    plat_init(argc, argv);
    LOG("Burnout 3 Wii recompilation: GPL-3.0-or-later, no warranty; see README.txt and CREDITS.md");
    {
        void ps2_sqrtf_selftest(void);
        ps2_sqrtf_selftest();
    }
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--trace") && i + 1 < argc)
            g_trace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-frames") && i + 1 < argc)
            g_dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--widescreen") && i + 1 < argc)
            g_widescreen = atoi(argv[++i]);     /* runtime/patches.c */
        else if (!strcmp(argv[i], "--vu") && i + 1 < argc) {
            /* interp: interpreter only; check: compare recompiled programs with it */
            extern int g_vu_mode;
            i++;
            g_vu_mode = !strcmp(argv[i], "interp") ? 0 : !strcmp(argv[i], "check") ? 2 : 1;
        }
#if !defined(PS2_RAM_FIXED_BASE)
    ps2_ram = plat_alloc_guest_ram(PS2_RAM_SIZE);
#endif
    disc_init();
    hw_init();
    kernel_init();
    sif_init();
    vu0_init();
    entry = elf_load("SLUS_210.50");
    game_patches_apply();               /* the data patches asked for (runtime/patches.c), before the game runs */
    LOG("entry %08X, %u recompiled functions", entry, ps2_func_count);
    kernel_start_main(entry);
    plat_sem_wait(plat_sem_create(0));   /* the guest threads run from here on */
    return 0;
}
