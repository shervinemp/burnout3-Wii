/* Padding between the program's own code and the libraries, sized by the
 * build (Makefile.wii, tools/wii/dspcheck.py) so that no library code lies
 * at 0x81000000..0x8100007F: libogc's start-up overwrites those bytes for a
 * moment to boot the DSP, and must not be running from them. */
    .section .text.zz_dsp_pad,"ax",@progbits
    .globl __dsp_pad
__dsp_pad:
#if PAD > 0
    .space PAD
#endif
    .globl __dsp_pad_end
__dsp_pad_end:
