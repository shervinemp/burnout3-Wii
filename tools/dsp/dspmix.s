/*
 * dspmix.s: the sound chip's voice mixer on the Wii's DSP (stage 2: ADPCM decoded here).
 *
 * A job is one voice for one chunk. The CPU walks the voice's place through its blocks as the chip does (end flags,
 * loops, interrupts) and lists the blocks the place enters as runs of consecutive blocks in sound RAM; the DSP takes
 * them by DMA itself and decodes them as runtime/iop_sd.c's decode_at does (the 16-byte PS2 ADPCM blocks: a header
 * with the shift and the filter, 28 four-bit samples, s = (n << 12 >> shift) + (p1 f0 + p2 f1 + 32) >> 6, stopped at
 * 16 bits). The voice's decoder - the block it is in (the sample before it first) and its last two samples - is its
 * record in main memory: read at the job's start, written back at its end, so the next run goes on from there. Then
 * the voice is resampled as voice_stretch does (a sample between two neighbours by the place's fraction, taken to
 * 8 bits), times the gains, and added into a sum per core. When the jobs are done the sums go back (a record a sample:
 * [mL, mR, lL, lR], the 32-bit sums being (m << 16 | l) in 1/32768 of a sample step; one that would pass twice full
 * scale stops at m = 0x7fff / 0x8000) with the mail 0xBEEF00nn (nn: runs so far). A core no job was for is not sent.
 *
 * Mails (CPU to DSP, 0xFACE00cc then the argument):
 *   0x80  the argument (31 bits) is the physical address of the command blocks (32-byte aligned), once
 *   0x10 + k (k 0..3)  run command block k, 1 KB past block k - 1: 16 header words, then up to 24 jobs of 16 words.
 *         A run mail may come while a run is going on: it waits in the box (one deep) for its turn.
 *
 * Command block (halfwords): header [0] jobs, [1] samples in the chunk (1..256), [2..3] result address (32-byte
 * aligned, 2 KB a core: core 1's result 2048 bytes on), [4..5] the records' address (record k 64 bytes on);
 * job: [0..1] the runs' list (32-byte aligned; a run is 4 words: the first block's address (16-byte aligned, sound
 * RAM in MEM2 or anywhere), blocks, spare), [2] runs, [3] core in bit 0, bit 8 left side on, bit 9 right side on,
 * [4] samples, [5] index of the first sample's left neighbour in the stream, [6] fraction at the start (16 bits),
 * [7] pitch's integer part, [8] its fraction (16 bits), [9] left gain at the start, [10] right gain (of 32768),
 * [11..12] left gain step a sample (32 bits, 16 fraction bits), [13..14] right gain step, [15] the voice's record.
 * A record (32 words): [0..28] the block the voice is in, the sample before it first, [29] p1, [30] p2.
 * The stream: the record's 29 samples, then 28 for each block entered (38 at most: 1093 words).
 *
 * Data memory (words): 0x000 the block's nibble table (16), 0x010 the job's fields, 0x020 the command block (400),
 * 0x1b0 variables, 0x1d0 the shifts' scales (16), 0x1e0 the filters (8 + 8), 0x1f0 four runs (16), 0x200 the resampled
 * voice (256), 0x300 the stream (1093 at most: to 0x744), 0x750 up to 8 blocks as they came (80), 0x7a0 the record
 * going back (32), 0x800 core 0's sums (256 records of 4), 0xc00 core 1's.
 */
DMACR:          equ     0xffc9
DMABLEN:        equ     0xffcb
DMADSPM:        equ     0xffcd
DMAMMEMH:       equ     0xffce
DMAMMEML:       equ     0xffcf

DIRQ:           equ     0xfffb
DMBH:           equ     0xfffc
DMBL:           equ     0xfffd
CMBH:           equ     0xfffe
CMBL:           equ     0xffff

DMA_TO_DSP:     equ     0x0
DMA_TO_CPU:     equ     0x1

V_CMDH:         equ     0x01b0
V_CMDL:         equ     0x01b1
V_JLEFT:        equ     0x01b2
V_CMASK:        equ     0x01b3
V_DONE:         equ     0x01b4
V_JPTR:         equ     0x01b5
V_SUMB:         equ     0x01b6
V_BYTES:        equ     0x01b7
V_RECH:         equ     0x01b8
V_RECL:         equ     0x01b9
V_OUT:          equ     0x01ba
V_NB:           equ     0x01bb
V_RLH:          equ     0x01bc
V_RLL:          equ     0x01bd
V_RLEFT:        equ     0x01be
V_RIN:          equ     0x01bf
V_ATH:          equ     0x01c0
V_ATL:          equ     0x01c1
V_BLEFT:        equ     0x01c2
V_PIECE:        equ     0x01c3
V_P1:           equ     0x01c4
V_P2:           equ     0x01c5
V_RPTR:         equ     0x01c6
V_SKIP:         equ     0x01c7

J_RLH:          equ     0x0010
J_RLL:          equ     0x0011
J_NRUNS:        equ     0x0012
J_COREF:        equ     0x0013
J_CNT:          equ     0x0014
J_IDX:          equ     0x0015
J_FRAC:         equ     0x0016
J_PI:           equ     0x0017
J_PF:           equ     0x0018
J_GL:           equ     0x0019
J_GR:           equ     0x001a
J_DLH:          equ     0x001b
J_DLL:          equ     0x001c
J_DRH:          equ     0x001d
J_DRL:          equ     0x001e
J_VOICE:        equ     0x001f

H_NJOBS:        equ     0x0020
H_M:            equ     0x0021
H_RESH:         equ     0x0022
H_RESL:         equ     0x0023
H_RECH:         equ     0x0024
H_RECL:         equ     0x0025
TABLE:          equ     0x0030
CMD_WORDS:      equ     400

NIB:            equ     0x0000
SCALE:          equ     0x01d0
F0:             equ     0x01e0
F1:             equ     0x01e8
RECBUF:         equ     0x07a0
RUNBUF:         equ     0x01f0
V_BASE:         equ     0x0200
IN_BASE:        equ     0x0300
COMP:           equ     0x0750
SUM_BASE:       equ     0x0800

_start:
        nop
        nop
        jmp     exception1
        jmp     exception2
        jmp     exception3
        jmp     exception4
        jmp     exception5
        jmp     exception6
        jmp     exception7

        sbset   #0x02
        sbset   #0x03
        sbclr   #0x04
        sbset   #0x05
        sbset   #0x06

        s16
        clr15
        m0
        lri     $config,#0xff

        lri     $wr0,#0xffff
        lri     $wr1,#0xffff
        lri     $wr2,#0xffff
        lri     $wr3,#0xffff

        lri     $acc0.m,#0
        sr      @V_DONE,$acc0.m
        // the decoder's tables: 2^(12 - shift) for each shift (above 12: 9, as decode_at has it), the filters
        lri     $acc0.m,#0x1000
        sr      @SCALE+0,$acc0.m
        lri     $acc0.m,#0x0800
        sr      @SCALE+1,$acc0.m
        lri     $acc0.m,#0x0400
        sr      @SCALE+2,$acc0.m
        lri     $acc0.m,#0x0200
        sr      @SCALE+3,$acc0.m
        lri     $acc0.m,#0x0100
        sr      @SCALE+4,$acc0.m
        lri     $acc0.m,#0x0080
        sr      @SCALE+5,$acc0.m
        lri     $acc0.m,#0x0040
        sr      @SCALE+6,$acc0.m
        lri     $acc0.m,#0x0020
        sr      @SCALE+7,$acc0.m
        lri     $acc0.m,#0x0010
        sr      @SCALE+8,$acc0.m
        lri     $acc0.m,#0x0008
        sr      @SCALE+9,$acc0.m
        lri     $acc0.m,#0x0004
        sr      @SCALE+10,$acc0.m
        lri     $acc0.m,#0x0002
        sr      @SCALE+11,$acc0.m
        lri     $acc0.m,#0x0001
        sr      @SCALE+12,$acc0.m
        lri     $acc0.m,#0x0008
        sr      @SCALE+13,$acc0.m
        lri     $acc0.m,#0x0008
        sr      @SCALE+14,$acc0.m
        lri     $acc0.m,#0x0008
        sr      @SCALE+15,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F0+0,$acc0.m
        lri     $acc0.m,#0x003c
        sr      @F0+1,$acc0.m
        lri     $acc0.m,#0x0073
        sr      @F0+2,$acc0.m
        lri     $acc0.m,#0x0062
        sr      @F0+3,$acc0.m
        lri     $acc0.m,#0x007a
        sr      @F0+4,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F0+5,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F0+6,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F0+7,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F1+0,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F1+1,$acc0.m
        lri     $acc0.m,#0xffcc
        sr      @F1+2,$acc0.m
        lri     $acc0.m,#0xffc9
        sr      @F1+3,$acc0.m
        lri     $acc0.m,#0xffc4
        sr      @F1+4,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F1+5,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F1+6,$acc0.m
        lri     $acc0.m,#0x0000
        sr      @F1+7,$acc0.m

main:
        si      @DMBH,#0xdcd1
        si      @DMBL,#0x0000
        si      @DIRQ,#0x01

recv_cmd:
        clr     $acc0
        clr     $acc1
        call    wait_mail_recv          // acc1.m = the box's high half
        lri     $acc0.m,#0xface
        cmp
        jeq     is_face
        lrs     $acc1.m,@CMBL           // not ours: read it so the box empties
        jmp     recv_cmd

is_face:
        lrs     $acc1.m,@CMBL
        cmpi    $acc1.m,#0x0080
        jeq     set_cmd
        mrr     $ix3,$acc1.m            // (0x0010 + the block's number)
        andi    $acc1.m,#0xfffc
        cmpi    $acc1.m,#0x0010
        jeq     run
        jmp     recv_cmd

set_cmd:
        call    wait_mail_recv
        andi    $acc1.m,#0x7fff
        sr      @V_CMDH,$acc1.m
        lrs     $acc0.m,@CMBL
        sr      @V_CMDL,$acc0.m
        jmp     recv_cmd

// ---- one run: the command block in, the jobs, the sums out
run:
        // the block's address: the base + k * 1024 (k << 16, shifted right by 6, added to the 40-bit base)
        clr     $acc1
        mrr     $acc1.m,$ix3
        andi    $acc1.m,#0x0003
        lsr     $acc1,#6
        clr     $acc0
        lr      $acc0.m,@V_CMDH
        lr      $acc0.l,@V_CMDL
        add     $acc0,$acc1
        lri     $ar0,#H_NJOBS
        lris    $acx1.l,#DMA_TO_DSP
        lri     $acx0.l,#CMD_WORDS*2
        call    dma_data

        // the sums to zero: 4 words a sample, rounded up to 4 samples, both cores
        clr     $acc0
        lr      $acc0.m,@H_M
        addi    $acc0.m,#3
        andi    $acc0.m,#0xfffc
        asl     $acc0,#2
        mrr     $ix3,$acc0.m
        clr     $acc0
        lri     $ar0,#SUM_BASE
        loop    $ix3
        srri    @$ar0,$acc0.m
        lri     $ar0,#SUM_BASE+0x400
        loop    $ix3
        srri    @$ar0,$acc0.m

        lri     $acc0.m,#TABLE
        sr      @V_JPTR,$acc0.m
        lri     $acc0.m,#0
        sr      @V_CMASK,$acc0.m
        lr      $acc0.m,@H_NJOBS
        sr      @V_JLEFT,$acc0.m

job_next:
        clr     $acc0
        lr      $acc0.m,@V_JLEFT
        cmpi    $acc0.m,#0
        jeq     jobs_done
        decm    $acc0.m
        sr      @V_JLEFT,$acc0.m
        lr      $acc1.m,@V_JPTR
        mrr     $ar1,$acc1.m
        addi    $acc1.m,#16
        sr      @V_JPTR,$acc1.m
        call    load_job
        call    decode_job
        call    run_job
        call    record_back
        jmp     job_next

jobs_done:
        // sums back: m4 samples of 8 bytes, each core that had a job
        clr     $acc0
        lr      $acc0.m,@H_M
        addi    $acc0.m,#3
        andi    $acc0.m,#0xfffc
        asl     $acc0,#3
        sr      @V_BYTES,$acc0.m
        lr      $acc1.m,@V_CMASK
        andf    $acc1.m,#0x0001
        jlz     out_core1
        lr      $acc0.m,@H_RESH
        lr      $acc0.l,@H_RESL
        lri     $ar0,#SUM_BASE
        lris    $acx1.l,#DMA_TO_CPU
        lr      $acx0.l,@V_BYTES
        call    dma_data
out_core1:
        lr      $acc1.m,@V_CMASK
        andf    $acc1.m,#0x0002
        jlz     out_done
        lr      $acc0.m,@H_RESH
        lr      $acc0.l,@H_RESL
        lri     $acx0.l,#0x0800
        addaxl  $acc0,$acx0
        lri     $ar0,#SUM_BASE+0x400
        lris    $acx1.l,#DMA_TO_CPU
        lr      $acx0.l,@V_BYTES
        call    dma_data
out_done:
        call    wait_mail_sent
        lr      $acc0.m,@V_DONE
        addis   $acc0.m,#1
        sr      @V_DONE,$acc0.m
        si      @DMBH,#0xbeef
        srs     @DMBL,$acc0.m
        jmp     recv_cmd

// ---- a job's 16 words (ar1 points at the first) into J_*
load_job:
        lri     $ar2,#J_RLH
        bloopi  #16,lj_end
        lrri    $acc1.m,@$ar1
lj_end:
        srri    @$ar2,$acc1.m
        ret

// ---- the job's stream: the voice's record (the block it is in, its decoder), then the blocks it enters, run by
// run, in pieces of up to 8 blocks by DMA, each decoded after the last
decode_job:
        clr     $acc0
        lr      $acc0.m,@H_RECH
        lr      $acc0.l,@H_RECL
        clr     $acc1
        lr      $acc1.m,@J_VOICE
        lsr     $acc1,#10
        add     $acc0,$acc1
        sr      @V_RECH,$acc0.m
        sr      @V_RECL,$acc0.l
        lri     $ar0,#IN_BASE
        lris    $acx1.l,#DMA_TO_DSP
        lri     $acx0.l,#64
        call    dma_data
        lr      $acc0.m,@IN_BASE+29
        sr      @V_P1,$acc0.m
        lr      $acc0.m,@IN_BASE+30
        sr      @V_P2,$acc0.m
        lri     $acc0.m,#IN_BASE+29
        sr      @V_OUT,$acc0.m
        lri     $acc0.m,#0
        sr      @V_NB,$acc0.m
        sr      @V_RIN,$acc0.m
        lr      $acc0.m,@J_NRUNS
        sr      @V_RLEFT,$acc0.m
        lr      $acc0.m,@J_RLH
        sr      @V_RLH,$acc0.m
        lr      $acc0.m,@J_RLL
        sr      @V_RLL,$acc0.m
dj_run:
        clr     $acc0
        lr      $acc0.m,@V_RLEFT
        cmpi    $acc0.m,#0
        jeq     dj_done
        decm    $acc0.m
        sr      @V_RLEFT,$acc0.m
        // the runs come in four at a time
        clr     $acc0
        lr      $acc0.m,@V_RIN
        cmpi    $acc0.m,#0
        jne     dj_have
        clr     $acc0
        lr      $acc0.m,@V_RLH
        lr      $acc0.l,@V_RLL
        lri     $ar0,#RUNBUF
        lris    $acx1.l,#DMA_TO_DSP
        lri     $acx0.l,#32
        call    dma_data
        clr     $acc0
        lr      $acc0.m,@V_RLH
        lr      $acc0.l,@V_RLL
        lri     $acx0.l,#32
        addaxl  $acc0,$acx0
        sr      @V_RLH,$acc0.m
        sr      @V_RLL,$acc0.l
        lri     $acc0.m,#4
        sr      @V_RIN,$acc0.m
        lri     $acc0.m,#RUNBUF
        sr      @V_RPTR,$acc0.m
dj_have:
        clr     $acc0
        lr      $acc0.m,@V_RIN
        decm    $acc0.m
        sr      @V_RIN,$acc0.m
        lr      $ar0,@V_RPTR
        lrri    $acc0.m,@$ar0
        sr      @V_ATH,$acc0.m
        lrri    $acc0.m,@$ar0
        sr      @V_ATL,$acc0.m
        lrri    $acc0.m,@$ar0
        sr      @V_BLEFT,$acc0.m
        iar     $ar0
        sr      @V_RPTR,$ar0
dj_piece:
        clr     $acc0
        lr      $acc0.m,@V_BLEFT
        cmpi    $acc0.m,#0
        jeq     dj_run
        cmpi    $acc0.m,#8
        jle     dj_small
        lri     $acc0.m,#8
dj_small:
        sr      @V_PIECE,$acc0.m
        clr     $acc1
        lr      $acc1.m,@V_BLEFT
        sub     $acc1,$acc0
        sr      @V_BLEFT,$acc1.m
        // the half block before the address comes too when it is not on 32 bytes: skipped (V_SKIP bytes)
        clr     $acc1
        lr      $acc1.m,@V_ATL
        andi    $acc1.m,#0x0010
        sr      @V_SKIP,$acc1.m
        // bytes: (skip + blocks * 16 + 31) rounded down to 32
        asl     $acc0,#4
        add     $acc0,$acc1
        addi    $acc0.m,#31
        andi    $acc0.m,#0xffe0
        mrr     $acx0.l,$acc0.m
        // from: the address rounded down to 32; the run's address goes on by the piece
        clr     $acc0
        lr      $acc0.m,@V_ATH
        lr      $acc0.l,@V_ATL
        clr     $acc1
        lr      $acc1.m,@V_PIECE
        lsr     $acc1,#12
        add     $acc1,$acc0
        sr      @V_ATH,$acc1.m
        sr      @V_ATL,$acc1.l
        clr     $acc1
        mrr     $acc1.m,$acc0.l
        andi    $acc1.m,#0xffe0
        mrr     $acc0.l,$acc1.m
        lri     $ar0,#COMP
        lris    $acx1.l,#DMA_TO_DSP
        call    dma_data
        // the piece's blocks
        clr     $acc0
        lr      $acc0.m,@V_SKIP
        lsr     $acc0,#1
        addi    $acc0.m,#COMP
        mrr     $ar2,$acc0.m
        lr      $ar3,@V_OUT
        lr      $acx0.h,@V_P1
        lr      $acx0.l,@V_P2
        lri     $ix3,#0x8000
dj_block:
        call    decode_block
        clr     $acc0
        lr      $acc0.m,@V_NB
        addis   $acc0.m,#1
        sr      @V_NB,$acc0.m
        clr     $acc0
        lr      $acc0.m,@V_PIECE
        decm    $acc0.m
        sr      @V_PIECE,$acc0.m
        cmpi    $acc0.m,#0
        jne     dj_block
        sr      @V_OUT,$ar3
        sr      @V_P1,$acx0.h
        sr      @V_P2,$acx0.l
        jmp     dj_piece
dj_done:
        ret

// ---- one block: ar2 at its 8 words, ar3 where its 28 samples go (both left past them); the decoder's last two
// samples in ax0.h (p1) and ax0.l (p2), ix3 = 0x8000. A sample: the nibble's entry (n << 12 >> shift) in m, 0x8000
// in l (the filter's +32, in 1024ths), plus (p1 f0 + p2 f1) << 10: m is then the sample with the 64ths taken to the
// floor, and the 40-bit mode's reads of m stop it at 16 bits, as decode_at does.
decode_block:
        s16
        clr     $acc0
        lrri    $acc0.m,@$ar2
        lsr     $acc0,#8
        clr     $acc1
        mrr     $acc1.m,$acc0.m
        andi    $acc0.m,#0x000f
        addi    $acc0.m,#SCALE
        mrr     $ar0,$acc0.m
        lsr     $acc1,#4
        andi    $acc1.m,#0x0007
        addi    $acc1.m,#F0
        mrr     $ar1,$acc1.m
        lrr     $acx1.h,@$ar1
        addi    $acc1.m,#8
        mrr     $ar1,$acc1.m
        lrr     $acx1.l,@$ar1
        // the nibbles' table: n times 2^(12 - shift), 0..7 and then -8..-1
        clr     $acc1
        lrr     $acc1.m,@$ar0
        clr     $acc0
        lri     $ar0,#NIB
        bloopi  #8,nb1_end
        srri    @$ar0,$acc0.m
nb1_end:
        add     $acc0,$acc1
        neg     $acc0
        bloopi  #8,nb2_end
        srri    @$ar0,$acc0.m
nb2_end:
        add     $acc0,$acc1
        // seven words of four samples: bits 8-11, 12-15, 0-3, 4-7 (each byte's low nibble first)
        bloopi  #7,dw_end
        lrri    $ix2,@$ar2
        s16
        clr     $acc0
        mrr     $acc0.m,$ix2
        lsr     $acc0,#8
        andi    $acc0.m,#0x000f
        mrr     $ar0,$acc0.m
        s40
        lrr     $acc0.m,@$ar0
        mrr     $acc0.l,$ix3
        mulx    $acx0.h,$acx1.h
        maddx   $acx0.l,$acx1.l
        movp    $acc1
        asl     $acc1,#10
        add     $acc0,$acc1
        mrr     $acx0.l,$acx0.h
        mrr     $acx0.h,$acc0.m
        srri    @$ar3,$acc0.m
        s16
        clr     $acc0
        mrr     $acc0.m,$ix2
        lsr     $acc0,#12
        andi    $acc0.m,#0x000f
        mrr     $ar0,$acc0.m
        s40
        lrr     $acc0.m,@$ar0
        mrr     $acc0.l,$ix3
        mulx    $acx0.h,$acx1.h
        maddx   $acx0.l,$acx1.l
        movp    $acc1
        asl     $acc1,#10
        add     $acc0,$acc1
        mrr     $acx0.l,$acx0.h
        mrr     $acx0.h,$acc0.m
        srri    @$ar3,$acc0.m
        s16
        clr     $acc0
        mrr     $acc0.m,$ix2
        andi    $acc0.m,#0x000f
        mrr     $ar0,$acc0.m
        s40
        lrr     $acc0.m,@$ar0
        mrr     $acc0.l,$ix3
        mulx    $acx0.h,$acx1.h
        maddx   $acx0.l,$acx1.l
        movp    $acc1
        asl     $acc1,#10
        add     $acc0,$acc1
        mrr     $acx0.l,$acx0.h
        mrr     $acx0.h,$acc0.m
        srri    @$ar3,$acc0.m
        s16
        clr     $acc0
        mrr     $acc0.m,$ix2
        lsr     $acc0,#4
        andi    $acc0.m,#0x000f
        mrr     $ar0,$acc0.m
        s40
        lrr     $acc0.m,@$ar0
        mrr     $acc0.l,$ix3
        mulx    $acx0.h,$acx1.h
        maddx   $acx0.l,$acx1.l
        movp    $acc1
        asl     $acc1,#10
        add     $acc0,$acc1
        mrr     $acx0.l,$acx0.h
        mrr     $acx0.h,$acc0.m
dw_end:
        srri    @$ar3,$acc0.m
        s16
        ret

// ---- the voice's decoder back to its record: the block it is in now (29 words of the stream), p1, p2
record_back:
        clr     $acc0
        lr      $acc0.m,@V_OUT
        addi    $acc0.m,#0xffe3
        mrr     $ar0,$acc0.m
        lri     $ar1,#RECBUF
        bloopi  #29,rb_end
        lrri    $acc1.m,@$ar0
rb_end:
        srri    @$ar1,$acc1.m
        lr      $acc1.m,@V_P1
        srri    @$ar1,$acc1.m
        lr      $acc1.m,@V_P2
        srri    @$ar1,$acc1.m
        lri     $acc1.m,#0
        srri    @$ar1,$acc1.m
        clr     $acc0
        lr      $acc0.m,@V_RECH
        lr      $acc0.l,@V_RECL
        lri     $ar0,#RECBUF
        lris    $acx1.l,#DMA_TO_CPU
        lri     $acx0.l,#64
        call    dma_data
        ret

run_job:
        clr     $acc0
        lr      $acc0.m,@J_COREF
        andi    $acc0.m,#0x0001
        lr      $acc1.m,@V_CMASK
        cmpi    $acc0.m,#0
        jeq     core_zero
        ori     $acc1.m,#0x0002
        jmp     core_marked
core_zero:
        ori     $acc1.m,#0x0001
core_marked:
        sr      @V_CMASK,$acc1.m
        asl     $acc0,#10
        addi    $acc0.m,#SUM_BASE
        sr      @V_SUMB,$acc0.m

        call    pass_resample

        lr      $acc0.m,@J_COREF
        andf    $acc0.m,#0x0100
        jlz     no_left
        call    pass_left
no_left:
        lr      $acc0.m,@J_COREF
        andf    $acc0.m,#0x0200
        jlz     no_right
        call    pass_right
no_right:
        ret

// ---- the voice at its pitch, 16 bits a sample, into V_BASE: v = s0 + (s1 - s0) * f / 256, f the place's
// fraction to 8 bits. acc0: the place (m the address of the sample's right neighbour, l the fraction); ax1.l the
// pitch's fraction, ax1.h its integer part; the products are doubled (m2) and the weight (in ax0.l, the sample in
// ax0.h) is f << 7, so the product is (s1 - s0) * f * 256 and its rounding to 16 bits is the correction to s0.
// (Only mul/msub/movpz/addr here: gcdsptool and Dolphin disagree on msubc's operand.)
pass_resample:
        m2
        lr      $ix3,@J_CNT
        lr      $acc0.m,@J_IDX
        addi    $acc0.m,#IN_BASE+1
        lr      $acc0.l,@J_FRAC
        lr      $acx1.l,@J_PF
        lr      $acx1.h,@J_PI
        lri     $ix0,#0xffff
        lri     $ar3,#V_BASE
        bloop   $ix3,rs_end
        mrr     $ar0,$acc0.m
        clr'ln  $acc1 : $acx0.h,@$ar0
        mrr     $acc1.m,$acc0.l
        lsr     $acc1,#1
        andi    $acc1.m,#0x7f80
        mrr     $acx0.l,$acc1.m
        mul'ln  $acx0.l,$acx0.h : $acx0.h,@$ar0
        msub    $acx0.l,$acx0.h
        movpz   $acc1
        addr    $acc1,$acx0.h
        addaxl's $acc0,$acx1 : @$ar3,$acc1.m
rs_end:
        addr    $acc0,$acx1.h
        m0
        ret

// ---- the voice times the left gain, added into the sums (m at +0, l at +2 of each record of 4). In the 40-bit mode
// a sum loaded through m is sign-extended (then l is loaded), and a store of m that does not fit 16 bits stops at
// 0x7fff / 0x8000: a sum past twice full scale stays there (l as it is) instead of wrapping round.
pass_left:
        s40
        lr      $ar1,@V_SUMB
        lr      $ar2,@V_SUMB
        iar     $ar2
        iar     $ar2
        lri     $ix1,#4
        lri     $ix2,#4
        lri     $ar3,#V_BASE
        lrri    $acx0.h,@$ar3
        lr      $acc0.m,@J_GL
        lri     $acc0.l,#0x8000         // (the gain taken to the nearest 1/32768, not down)
        lr      $acx1.h,@J_DLH
        lr      $acx1.l,@J_DLL
        lr      $ix3,@J_CNT
        bloop   $ix3,pl_end
        lrr     $acc1.m,@$ar1
        lrr     $acc1.l,@$ar2
        mulc'l  $acc0.m,$acx0.h : $acx0.h,@$ar3
        addp    $acc1
        addax'sn $acc0,$acx1 : @$ar1,$acc1.m
pl_end:
        nx'sn   : @$ar2,$acc1.l
        s16
        ret

// ---- the same for the right side (m at +1, l at +3)
pass_right:
        s40
        lr      $ar1,@V_SUMB
        iar     $ar1
        lr      $ar2,@V_SUMB
        iar     $ar2
        iar     $ar2
        iar     $ar2
        lri     $ix1,#4
        lri     $ix2,#4
        lri     $ar3,#V_BASE
        lrri    $acx0.h,@$ar3
        lr      $acc0.m,@J_GR
        lri     $acc0.l,#0x8000
        lr      $acx1.h,@J_DRH
        lr      $acx1.l,@J_DRL
        lr      $ix3,@J_CNT
        bloop   $ix3,pr_end
        lrr     $acc1.m,@$ar1
        lrr     $acc1.l,@$ar2
        mulc'l  $acc0.m,$acx0.h : $acx0.h,@$ar3
        addp    $acc1
        addax'sn $acc0,$acx1 : @$ar1,$acc1.m
pr_end:
        nx'sn   : @$ar2,$acc1.l
        s16
        ret

dma_data:
        srs     @DMAMMEMH,$acc0.m
        srs     @DMAMMEML,$acc0.l
        sr      @DMADSPM,$ar0
        sr      @DMACR,$acx1.l
        sr      @DMABLEN,$acx0.l
wait_dma:
        lrs     $acc0.m,@DMACR
        andf    $acc0.m,#0x04
        jlnz    wait_dma
        ret

wait_mail_sent:
        lrs     $acc0.m,@DMBH
        andf    $acc0.m,#0x8000
        jlnz    wait_mail_sent
        ret

wait_mail_recv:
        lrs     $acc1.m,@CMBH
        andcf   $acc1.m,#0x8000
        jlnz    wait_mail_recv
        ret

exception1:
        rti
exception2:
        rti
exception3:
        rti
exception4:
        rti
exception5:
        rti
exception6:
        rti
exception7:
        rti
