/*
 * toy.s: the plumbing test for the Wii DSP (stage 0 of moving the sound chip's mixer to the DSP).
 *
 * What it proves, against libogc's DSP_AddTask and the mail boxes:
 *   - the task boots (the DSP ROM loads this image into IRAM at 0 and enters at 0x10),
 *   - the init mail 0xDCD10000 with the interrupt reaches the task's init callback,
 *   - 32-bit mails both ways (the CPU's high half is only 15 bits: bit 15 of it is the box's "full" flag),
 *   - DMA from main memory into data memory and back (MEM1, and MEM2 to see whether the DSP reaches it),
 *   - 16-bit arithmetic on a block.
 *
 * Commands (CPU mail 0xFACE00cc, then a second mail with the argument):
 *   0x80  the argument's 31 bits (high 15, low 16) are the physical address of a block of 64 words
 *   0x10  the argument's low half is k: the block is DMAed in, k is added to every word, DMAed back,
 *         and the DSP mails 0xBEEFnnnn (nnnn: the number of runs so far; no interrupt, the CPU polls)
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

VAR_ADDRH:      equ     0x0000
VAR_ADDRL:      equ     0x0001
VAR_COUNT:      equ     0x0002
VAR_K:          equ     0x0003
BUF:            equ     0x0100
NWORDS:         equ     64

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
        sr      @VAR_COUNT,$acc0.m

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
        jeq     set_addr
        cmpi    $acc1.m,#0x0010
        jeq     run
        jmp     recv_cmd

set_addr:
        call    wait_mail_recv
        andi    $acc1.m,#0x7fff
        sr      @VAR_ADDRH,$acc1.m
        lrs     $acc0.m,@CMBL
        sr      @VAR_ADDRL,$acc0.m
        jmp     recv_cmd

run:
        call    wait_mail_recv
        lrs     $acc0.m,@CMBL
        sr      @VAR_K,$acc0.m

        lr      $acc0.m,@VAR_ADDRH      // block in
        lr      $acc0.l,@VAR_ADDRL
        lri     $ar0,#BUF
        lris    $acx1.l,#DMA_TO_DSP
        lri     $acx0.l,#NWORDS*2
        call    dma_data

        lri     $ar0,#BUF
        lri     $ar1,#BUF
        lr      $acc1.m,@VAR_K
        lri     $acx0.l,#NWORDS
        bloop   $acx0.l,run_loop_end
        lrri    $acc0.m,@$ar0
        add     $acc0,$acc1
run_loop_end:
        srri    @$ar1,$acc0.m

        lr      $acc0.m,@VAR_ADDRH      // block out
        lr      $acc0.l,@VAR_ADDRL
        lri     $ar0,#BUF
        lris    $acx1.l,#DMA_TO_CPU
        lri     $acx0.l,#NWORDS*2
        call    dma_data

        call    wait_mail_sent
        lr      $acc0.m,@VAR_COUNT
        addis   $acc0.m,#1
        sr      @VAR_COUNT,$acc0.m
        si      @DMBH,#0xbeef
        srs     @DMBL,$acc0.m
        jmp     recv_cmd

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
