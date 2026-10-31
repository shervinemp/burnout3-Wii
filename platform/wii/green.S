# Context switch for the Wii host threads (see plat_wii.c), PowerPC EABI.
#
#   void green_switch(void **save_sp, void *new_sp)
#
# Saves the callee-saved state (r13-r31, f14-f31, CR, LR) in a frame on the
# current stack, stores the stack pointer to *save_sp and resumes the context
# whose frame is at new_sp. Only the double (ps0) half of the FPRs is kept:
# compiled C does not hold paired singles across calls. r2 (small data base)
# and FPSCR are the same for every context.

    .set FRAME, 240

    .text
    .align 2
    .globl green_switch
    .type green_switch, @function
green_switch:
    stwu    1, -FRAME(1)
    mflr    0
    stw     0, FRAME+4(1)
    mfcr    12
    stw     12, 8(1)
    stmw    13, 12(1)           # r13-r31: 12..87
    stfd    14, 88(1)
    stfd    15, 96(1)
    stfd    16, 104(1)
    stfd    17, 112(1)
    stfd    18, 120(1)
    stfd    19, 128(1)
    stfd    20, 136(1)
    stfd    21, 144(1)
    stfd    22, 152(1)
    stfd    23, 160(1)
    stfd    24, 168(1)
    stfd    25, 176(1)
    stfd    26, 184(1)
    stfd    27, 192(1)
    stfd    28, 200(1)
    stfd    29, 208(1)
    stfd    30, 216(1)
    stfd    31, 224(1)
    stw     1, 0(3)
    mr      1, 4
    lfd     14, 88(1)
    lfd     15, 96(1)
    lfd     16, 104(1)
    lfd     17, 112(1)
    lfd     18, 120(1)
    lfd     19, 128(1)
    lfd     20, 136(1)
    lfd     21, 144(1)
    lfd     22, 152(1)
    lfd     23, 160(1)
    lfd     24, 168(1)
    lfd     25, 176(1)
    lfd     26, 184(1)
    lfd     27, 192(1)
    lfd     28, 200(1)
    lfd     29, 208(1)
    lfd     30, 216(1)
    lfd     31, 224(1)
    lmw     13, 12(1)
    lwz     12, 8(1)
    mtcr    12
    lwz     0, FRAME+4(1)
    mtlr    0
    addi    1, 1, FRAME
    blr
    .size green_switch, .-green_switch

# First entry of a new context: green_new() leaves the context pointer in r31
# and this address as the saved LR.
    .globl green_start
    .type green_start, @function
green_start:
    mr      3, 31
    b       green_main
    .size green_start, .-green_start
