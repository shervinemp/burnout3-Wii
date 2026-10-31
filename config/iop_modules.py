"""IOP modules from the disc that are statically recompiled (see tools/recomp --iop).

Each module is relocated to a fixed IOP address. BIOS modules and the
hardware-facing ones (cdvdman, libsd, ...) are not recompiled: their
exports are implemented natively in runtime/iop_*.c.

Addresses in JUMP_TABLES are module-relative: jr pc -> (table address, entries).
"""

MODULES = {
    "rwa": {
        "file": "IOP/RWA.IRX",           # RenderWare Audio driver (SPU2 through libsd)
        "base": 0x00040000,
        "jump_tables": {0xA16C: (0xAAA0, 8)},    # unrolled copy loop (Duff's device)
        # module-relative functions whose calls are logged (debugging only)
        "debug_wrap": [],
    },
    "b3route": {
        "file": "IOP/B3ROUTE.IRX",       # Burnout 3 audio distortion router
        "base": 0x00058000,
    },
    "gtfscdvd": {
        "file": "IOP/GTFSCDVD.IRX",      # Criterion's GTFS file system over cdvdman
        "base": 0x00060000,
    },
    # Pads and memory cards (libpad2 / libmc2). These run on an emulated SIO2
    # controller (runtime/iop_sio2.c) and link to each other's exports.
    "sio2man": {
        "file": "IOP/SIO2MAN.IRX",       # SIO2 transfers
        "base": 0x0006C000,
    },
    "sio2d": {
        "file": "IOP/SIO2D.IRX",         # SIO2 device scheduling
        "base": 0x0006E000,
    },
    "dbcman": {
        "file": "IOP/DBCMAN.IRX",        # device base controller manager (libdbc's RPC server)
        "base": 0x00072000,
    },
    "ds2o": {
        "file": "IOP/DS2O.IRX",          # DualShock 2 driver
        "base": 0x0007A000,
    },
    # The game loads DS2O twice: each copy registers one pad with DBCMAN (no
    # exports, so the second stays resident), and the second drives port 2.
    # LOADFILE takes the first image of a name that is not loaded yet.
    "ds2o_2": {
        "file": "IOP/DS2O.IRX",
        "base": 0x00084000,
    },
    "mc2_s1": {
        "file": "IOP/MC2_S1.IRX",        # memory card file system (libmc2)
        "base": 0x0007E000,
    },
}
