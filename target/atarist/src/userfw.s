; Copyright (C) 2026 Neil Rackett
; SPDX-License-Identifier: GPL-3.0-or-later

; MD/Snap user firmware: resident screen grabber.
; (C) 2026 Neil Rackett
; License: GPL v3
;
; Installed once (via main.s rom_function) when the RP raises CMD_START, and
; re-asserted on the way out to the desktop from boot_gem. It survives into the
; running desktop and captures the screen on demand.
;
; Mechanism
; ---------
; The RP bumps the LSB of shared-var slot 3 (CAPTURE_SEQ_ADDR, $FA201F) on each
; SELECT short-press. A resident handler polls that byte; when it changes it
; snapshots the screen + palette + rez + machine type into a RAM buffer, then
; dribbles the 32000-byte screen out the ROM3 window to the RP, one 2000-byte
; chunk per tick (16 chunks + 1 metadata command). The RP deplanes,
; pixel-doubles and writes an indexed PNG.
;
; TT
; --
; On a TT (_MCH $0002xxxx) the "rez" sent is the whole TT shifter mode word
; ($FF8262: mode in bits 8-10, ST palette bank in bits 0-3) and the palette is
; all 256 TT colour words ($FF8400). ST low/medium/high on a TT are still 32000
; bytes and go out exactly as on an ST; TT low/medium/high are 153600 bytes and
; go out as 80 x 1920-byte chunks (VBL) or 160 x 960-byte frames (ETV), each a
; whole number of screen rows so the RP can write PNG rows as they arrive.
;
; VBL hook
; --------
; The legacy fast path hooks the VBL autovector ($70). This path is deliberately
; kept identical in shape to the known-good implementation; ETV work must not
; run this blocking transport from $400.
;
; ETV hook
; --------
; The experimental ETV path hooks etv_timer ($400), snapshots the screen once,
; then emits one 500-byte async stream frame per timer callback. It never waits
; for RP acknowledgement inside the timer hook; instead it polls shared-var slot
; 5 (STREAM_ACK_ADDR) for credit before sending the next frame.
;
; Why copy to RAM
; ---------------
; The whole cartridge region ($FA0000-$FAFFFF) is emulated read-only to the ST:
; code can be fetched/read from it, but stores bus-error. The resident handler
; needs writable state, so the installer Malloc's a RAM block and copies the
; position-independent handler into it. The big screen snapshot buffer is NOT
; assembled into the cartridge image (that would blow the 8 KB budget) -- it is
; the extra space Malloc'd just past the resident block, reached via a pointer
; the installer stores into the block. Reads of the shared region (the capture
; counter) still work, so the handler reads slot 3 in place; and the write
; transport in main.s is reached by an absolute jsr (a PC-relative bsr from the
; RAM copy could not reach ROM).

	section text

GEMDOS_Malloc		equ 72			; trap #1 -> allocate, ptr in d0
GEMDOS_Mxalloc		equ 68			; trap #1 -> allocate by RAM type (TOS 3+)
MXALLOC_ST_ONLY		equ 0			; ST-RAM only
VBL_VECTOR			equ $70			; .l level-4 (VBL) autovector
ETV_TIMER_VECTOR	equ $400		; .l etv_timer subroutine called by TOS
_p_cookies			equ $5a0		; .l pointer to the system cookie jar (0 = none)

; Hardware registers (long-absolute; the 68000 truncates to $FF82xx).
SHIFTER_REZ			equ $FFFF8260	; resolution: 0=low, 1=med, 2=high (low 2 bits)
PALETTE				equ $FFFF8240	; 16 colour words
VID_BASE_HI			equ $FFFF8201	; video base address, bits 16-23
VID_BASE_MID		equ $FFFF8203	; video base address, bits 8-15
									; bits 0-7 ($FF820D) are STE-only and 0 for
									; normal desktop output, so we assume 0
									; (except on a TT, which reads it).
VID_BASE_LO			equ $FFFF820D	; video base address, bits 0-7 (TT: bits 3-7)
TT_SHIFTER_MODE		equ $FFFF8262	; TT only: mode bits 8-10, bank bits 0-3
TT_PALETTE			equ $FFFF8400	; TT only: 256 colour words, 0RGB
TT_MODE_TT_MED		equ 4			; TT modes 4 (med), 6 (high), 7 (low) are
									; the 153600-byte ones; 0-2 are ST modes
MCH_TT				equ 2			; _MCH high word for a TT
MCH_FALCON			equ 3			; _MCH high word for a Falcon
; A Falcon's Videl (falcon_rez).
FALCON_MONITOR		equ $FFFF8006	; bits 6-7: 0 = mono (SM124)
VIDEL_LINE_OFFSET	equ $FFFF820E	; words skipped after each line
VIDEL_LINE_WIDTH	equ $FFFF8210	; words per line
FALCON_SHIFT		equ $FFFF8266	; bit 10: 1 bit, 8: 16 bits, 4: 8 bits
VIDEL_VDB			equ $FFFF82A8	; vertical display begin (half lines)
VIDEL_VDE			equ $FFFF82AA	; vertical display end
VIDEL_VCO			equ $FFFF82C2	; bit 0: lines doubled, bit 1: interlaced
; Sent as the "rez" of a screen MD/Snap can't capture (one of a Falcon's own
; modes): the RP saves nothing and flashes the LED to say so. No machine's
; mode reads as this (screenshot.c SHOT_REZ_UNSUPPORTED).
REZ_UNSUPPORTED		equ $FFFF
CPU_68030			equ 30			; _CPU cookie value for a 68030 (TT, Falcon)
CACR_ED				equ 8			; 68030 CACR: enable data cache
CACR_CD				equ 11			; 68030 CACR: clear data cache
MEGASTE_CTRL_ADDR	equ $FFFF8E21	; Mega STE CPU control byte
MEGASTE_CTRL_CACHE_BIT	equ $01		; canonical MSTE_CC values use bit0 as cache
MCH_MEGA_STE		equ $00010010	; _MCH cookie value for Mega STE

; Shared region / protocol (must match main.s).
CAPTURE_SEQ_ADDR	equ $FA201F		; LSB of shared-var slot 3 (RP bumps it)
HOOK_FLAG_ADDR		equ $FA2023		; LSB of shared-var slot 4 (0=VBL, 1=ETV)
STREAM_ACK_ADDR		equ $FA2024		; shared-var slot 5: seq<<16 | next frame
RANDOM_TOKEN_SEED_ADDR	equ $FA2008
ROMCMD_START_ADDR	equ $FB0000
CMD_MAGIC_NUMBER	equ $ABCD
APP_SCREENSHOT_BEGIN	equ $10
APP_SCREENSHOT_DATA		equ $11
APP_SCREENSHOT_STREAM_BEGIN	equ $12
APP_SCREENSHOT_STREAM_DATA	equ $13
SHOT_CHUNK_SIZE		equ 2000
SHOT_NUM_CHUNKS		equ 16
SHOT_SCREEN_SIZE	equ 32000
SHOT_STREAM_FRAME_SIZE	equ 500
SHOT_STREAM_FRAME_COUNT	equ 64
SHOT_PAL_BYTES		equ 32			; ST/STE: 16 words from $FF8240
TT_PAL_BYTES		equ 512			; TT: 256 words from $FF8400
TT_SCREEN_SIZE		equ 153600		; TT low 320x480x8, med 640x480x4, high 1280x960x1
TT_CHUNK_SIZE		equ 1920		; VBL: 80 chunks (6 rows of 320 / 12 of 160)
TT_NUM_CHUNKS		equ 80
TT_STREAM_FRAME_SIZE	equ 960		; ETV: 160 frames (3 rows of 320 / 6 of 160)
TT_STREAM_FRAME_COUNT	equ 160
RES_RETRIES			equ 3			; transport retries per chunk
; TOS calls etv_timer on every fourth 200 Hz timer C interrupt: every 20 ms
; (_timr_ms), 50 times a second.
ETV_RETRY_TICKS	equ 5			; 5 * 20 ms = 100 ms
ETV_ABORT_TICKS	equ 200			; 200 * 20 ms = 4 seconds

; The write transport lives in main.s (ROM). Imported so the resident RAM copy
; can call it via an absolute-address jsr.
	xref send_sync_write_command_to_sidecart

; -----------------------------------------------------------------------
; Installer -- runs from ROM at USERFW ($FA0800). Only writes to RAM.
; GEMDOS/XBIOS preserve d3-d7/a3-a6, so a3 (the block base) and d3 (the
; snapshot buffer size) survive the traps.
; -----------------------------------------------------------------------
userfw:
	movem.l	d0-d3/a0-a3, -(sp)

	; Allocate the resident block + the trailing screen snapshot buffer + the
	; palette buffer. A TT needs room for its 153600-byte screens and 256-colour
	; palette. It is taken from ST-RAM only (Mxalloc, TOS 3+), never TT-RAM:
	; TOS 3 tests TT-RAM after the cartridge has installed this, writing its
	; test patterns over the resident handler, which the next VBL then runs
	; (11 bombs: the patterns run as line-F instructions).
	bsr		read_mch				; d0 = _MCH (runs from ROM: reads only)
	swap	d0
	cmp.w	#MCH_TT, d0
	beq.s	.uf_alloc_tt
	move.l	#((resident_end-resident_start)+SHOT_SCREEN_SIZE+SHOT_PAL_BYTES), -(sp)
	move.w	#GEMDOS_Malloc, -(sp)
	trap	#1
	addq.l	#6, sp
	move.l	#SHOT_SCREEN_SIZE, d3
	bra.s	.uf_alloc_done
.uf_alloc_tt:
	move.w	#MXALLOC_ST_ONLY, -(sp)
	move.l	#((resident_end-resident_start)+TT_SCREEN_SIZE+TT_PAL_BYTES), -(sp)
	move.w	#GEMDOS_Mxalloc, -(sp)
	trap	#1
	addq.l	#8, sp
	move.l	#TT_SCREEN_SIZE, d3
.uf_alloc_done:
	tst.l	d0
	beq		.uf_exit				; out of memory -> install nothing
	movea.l	d0, a3					; a3 = RAM block base (survives traps)

	; Copy the resident code+state (NOT the trailing snapshot buffer) into RAM.
	lea		resident_start(pc), a0
	movea.l	a3, a1
	move.w	#(resident_end-resident_start-1), d1
.uf_copy:
	move.b	(a0)+, (a1)+
	dbf		d1, .uf_copy

	; The snapshot buffer sits right after the copied block, and the palette
	; buffer right after that; store their addresses into the block so the
	; resident code can find them.
	move.l	a3, d0
	add.l	#(resident_end-resident_start), d0
	move.l	d0, (res_screenptr-resident_start)(a3)
	add.l	d3, d0
	move.l	d0, (res_palptr-resident_start)(a3)

	; A 68030 (TT, Falcon) has a data cache that can serve the RP's shared
	; values in the cartridge window from an earlier read; flag it so the
	; resident code keeps those reads fresh (dcache_flush, cache_guard_*).
	move.l	#'_CPU', d2
	bsr		read_cookie
	cmp.l	#CPU_68030, d0
	bne.s	.uf_not_030
	move.b	#1, (res_030-resident_start)(a3)
.uf_not_030:

	; Start idle, and seed res_prev with the current counter so a stale value
	; can't fire a capture the instant we install.
	clr.b	(res_state-resident_start)(a3)
	move.b	CAPTURE_SEQ_ADDR, (res_prev-resident_start)(a3)

	; Hook either the known-good VBL entry or the separate async ETV entry.
	moveq	#0, d0
	move.b	HOOK_FLAG_ADDR, d0
	cmp.b	#1, d0
	beq.s	.uf_hook_etv

	; VBL: save the original into the block, point $70 at resident_start
	; (block offset 0). Mask interrupts across the swap. $70 is supervisor-only;
	; the installer runs supervisor at boot, so the access is legal.
	move.w	sr, -(sp)
	ori.w	#$0700, sr
	move.l	VBL_VECTOR.w, (orig_hook-resident_start)(a3)
	move.l	a3, VBL_VECTOR.w
	move.w	(sp)+, sr
	bra.s	.uf_exit

.uf_hook_etv:
	move.l	a3, d0
	add.l	#(resident_etv_start-resident_start), d0
	move.w	sr, -(sp)
	ori.w	#$0700, sr
	move.l	ETV_TIMER_VECTOR.w, (orig_hook-resident_start)(a3)
	move.l	d0, ETV_TIMER_VECTOR.w
	move.w	(sp)+, sr

.uf_exit:
	movem.l	(sp)+, d0-d3/a0-a3
	rts

; -----------------------------------------------------------------------
; Resident block -- copied verbatim into RAM and run from there. Position
; independent: it references its own data/subroutines PC-relative, the shared
; region by absolute address, and the ROM transport by absolute jsr. Entry
; (offset 0) is installed at $70: it runs at VBL interrupt level, preserves
; every register it touches, and tail-chains into the saved original.
; -----------------------------------------------------------------------
	even
resident_start:
	movem.l	d0-d7/a0-a6, -(sp)
	bsr		dcache_flush

	move.b	res_state(pc), d0
	bne.s	.active					; mid-transfer -> keep dribbling

	; Idle: has a new capture been requested?
	move.b	CAPTURE_SEQ_ADDR, d0	; read shared region (reads are OK)
	lea		res_prev(pc), a0
	cmp.b	(a0), d0
	beq.s	.chain					; unchanged -> nothing to do
	move.b	d0, (a0)				; remember the new value
	bsr		snapshot				; grab screen + palette + rez + machine type
	lea		res_state(pc), a0
	move.b	#1, (a0)				; next tick starts sending (state 1 = BEGIN)
	bra.s	.chain

.active:
	cmp.b	#1, d0
	bne.s	.send_data
	bsr		send_begin
	bra.s	.advance
.send_data:
	moveq	#0, d1
	move.b	d0, d1
	subq.l	#2, d1					; d1 = chunk index 0..SHOT_NUM_CHUNKS-1
	bsr		send_chunk
.advance:
	lea		res_state(pc), a0
	move.b	(a0), d0
	addq.b	#1, d0
	move.w	res_nchunks(pc), d1
	addq.w	#2, d1
	cmp.b	d1, d0					; 1 (begin) + N (data) done after state N+1
	bne.s	.store_state
	moveq	#0, d0					; finished -> back to idle
.store_state:
	move.b	d0, (a0)

.chain:
	movem.l	(sp)+, d0-d7/a0-a6
	move.l	orig_hook(pc), -(sp)	; chain into the saved original handler...
	rts								; VBL original rte's, popping the interrupt frame

; --- Snapshot the current screen state into the RAM block ---------------
; Also sets the sizes the send paths use for this capture: screen bytes,
; palette bytes, and the VBL chunk / ETV frame size and count.
snapshot:
	; Machine type from the _MCH cookie (high word: 0=ST, 1=STE, 2=TT...).
	bsr		read_mch				; -> d0
	lea		res_mch(pc), a0
	move.l	d0, (a0)
	swap	d0
	cmp.w	#MCH_TT, d0
	beq.s	.snap_tt
	cmp.w	#MCH_FALCON, d0
	bne.s	.snap_st_rez
	bsr		falcon_rez				; d0 = ST mode 0-2, or -1: the Falcon's own
	tst.w	d0
	bpl.s	.snap_rez
	bra		snap_unsupported

.snap_st_rez:
	; Resolution (low 2 bits of the Shifter resolution register).
	moveq	#0, d0
	move.b	SHIFTER_REZ, d0
	and.w	#3, d0
.snap_rez:
	lea		res_rez(pc), a0
	move.l	d0, (a0)

	; Palette: 16 words.
	movea.l	#PALETTE, a0
	move.l	res_palptr(pc), a1
	moveq	#15, d0
.snap_pal:
	move.w	(a0)+, (a1)+
	dbf		d0, .snap_pal
	move.w	#SHOT_PAL_BYTES, d2

	; Screen base = HI<<16 | MID<<8 (low byte assumed 0).
	moveq	#0, d0
	move.b	VID_BASE_HI, d0
	lsl.l	#8, d0
	move.b	VID_BASE_MID, d0
	lsl.l	#8, d0
	bra.s	.snap_st_size

.snap_tt:
	; TT: the whole shifter mode word and all 256 palette words; the RP works
	; out the bank / colours for the mode.
	moveq	#0, d0
	move.w	TT_SHIFTER_MODE, d0
	lea		res_rez(pc), a0
	move.l	d0, (a0)
	movea.l	#TT_PALETTE, a0
	move.l	res_palptr(pc), a1
	move.w	#255, d1
.snap_tt_pal:
	move.w	(a0)+, (a1)+
	dbf		d1, .snap_tt_pal
	move.w	#TT_PAL_BYTES, d2

	; Screen base = HI<<16 | MID<<8 | LO (a TT's base is 8-byte aligned).
	lsr.w	#8, d0
	and.w	#7, d0					; d0 = TT mode
	move.w	d0, d1
	moveq	#0, d0
	move.b	VID_BASE_HI, d0
	lsl.l	#8, d0
	move.b	VID_BASE_MID, d0
	lsl.l	#8, d0
	move.b	VID_BASE_LO, d0
	and.b	#$f8, d0
	cmp.w	#TT_MODE_TT_MED, d1
	blo.s	.snap_st_size			; ST low/medium/high on a TT

	move.l	#TT_SCREEN_SIZE, d1
	move.w	#TT_CHUNK_SIZE, d3
	move.w	#TT_NUM_CHUNKS, d4
	move.w	#TT_STREAM_FRAME_SIZE, d5
	move.w	#TT_STREAM_FRAME_COUNT, d6
	bra.s	.snap_store

.snap_st_size:
	move.l	#SHOT_SCREEN_SIZE, d1
	move.w	#SHOT_CHUNK_SIZE, d3
	move.w	#SHOT_NUM_CHUNKS, d4
	move.w	#SHOT_STREAM_FRAME_SIZE, d5
	move.w	#SHOT_STREAM_FRAME_COUNT, d6

.snap_store:
	lea		res_palbytes(pc), a0
	move.w	d2, (a0)+				; res_palbytes, res_chunk, res_nchunks,
	move.w	d3, (a0)+				; res_frame, res_nframes are consecutive
	move.w	d4, (a0)+
	move.w	d5, (a0)+
	move.w	d6, (a0)

	movea.l	d0, a0					; a0 = screen base
	move.l	res_screenptr(pc), a1	; a1 = snapshot buffer
	lsr.l	#2, d1
	subq.l	#1, d1					; longs - 1 (<= 38399, fits dbf)
.snap_copy:
	move.l	(a0)+, (a1)+
	dbf		d1, .snap_copy
	rts

; A screen MD/Snap can't capture: only the BEGIN goes, saying so, and no
; screen (no chunks, no frames). The palette size stays a real one, as the
; transport sends that many bytes.
snap_unsupported:
	lea		res_rez(pc), a0
	move.l	#REZ_UNSUPPORTED, (a0)
	lea		res_palbytes(pc), a0
	move.w	#SHOT_PAL_BYTES, (a0)+	; res_palbytes
	clr.w	(a0)+					; res_chunk
	clr.w	(a0)+					; res_nchunks
	clr.w	(a0)+					; res_frame
	clr.w	(a0)					; res_nframes
	rts

; --- The ST mode a Falcon's Videl is showing ------------------------------
; d0 = 0, 1 or 2 when it is ST low, medium or high, as the Falcon shows them
; for ST programs; -1 for one of its own modes (another size or depth),
; which MD/Snap can't capture. The depth is as the Videl takes it: Falcon
; shift mode bits 10 (1 bit), 8 (16 bits) and 4 (8 bits) first, else the ST
; shifter's. The width is the line width; the height is VDE - VDB, in half
; lines unless interlaced, halved again when lines are doubled (within 8
; lines). A mono monitor shows only 640x400. Uses d0-d2.
falcon_rez:
	move.w	FALCON_SHIFT, d0
	btst	#10, d0
	bne.s	.fr_1bit
	and.w	#$0110, d0
	bne		.fr_no					; 8 or 16 bits
	moveq	#0, d0
	move.b	SHIFTER_REZ, d0
	and.w	#3, d0
	cmp.w	#2, d0
	bhi		.fr_no
	bra.s	.fr_size
.fr_1bit:
	moveq	#2, d0
.fr_size:
	move.w	VIDEL_LINE_OFFSET, d1
	and.w	#$01FF, d1
	bne		.fr_no					; a screen wider than shown
	moveq	#80, d1					; words a line: 320x4, 640x2...
	cmp.w	#2, d0
	bne.s	.fr_width
	moveq	#40, d1					; ...640x1
.fr_width:
	move.w	VIDEL_LINE_WIDTH, d2
	and.w	#$03FF, d2
	cmp.w	d1, d2
	bne		.fr_no
	move.b	FALCON_MONITOR, d1
	and.b	#$C0, d1
	beq.s	.fr_yes					; mono: 640x400
	move.w	VIDEL_VDE, d1
	and.w	#$07FF, d1
	move.w	VIDEL_VDB, d2
	and.w	#$07FF, d2
	sub.w	d2, d1
	bmi		.fr_no
	move.w	VIDEL_VCO, d2
	btst	#1, d2
	bne.s	.fr_lines
	lsr.w	#1, d1					; half lines -> lines
.fr_lines:
	btst	#0, d2
	beq.s	.fr_height
	lsr.w	#1, d1					; each line shown twice
.fr_height:
	move.w	#200, d2
	cmp.w	#2, d0
	bne.s	.fr_expect
	move.w	#400, d2
.fr_expect:
	sub.w	d2, d1
	bpl.s	.fr_near
	neg.w	d1
.fr_near:
	cmp.w	#8, d1
	bhi		.fr_no
.fr_yes:
	rts
.fr_no:
	moveq	#-1, d0
	rts

; --- Read the _MCH cookie value into d0 (0 = assume plain ST) -----------
read_mch:
	move.l	#'_MCH', d2
; --- Read cookie d2's value into d0 (0 = no such cookie). Uses d1/a0 -----
read_cookie:
	moveq	#0, d0
	move.l	_p_cookies.w, d1
	beq.s	.mch_done
	movea.l	d1, a0
.mch_loop:
	move.l	(a0)+, d1
	beq.s	.mch_done				; 0 = end of jar
	cmp.l	d2, d1
	beq.s	.mch_found
	addq.l	#4, a0					; skip the value, try next cookie
	bra.s	.mch_loop
.mch_found:
	move.l	(a0), d0				; the cookie's value
.mch_done:
	rts

; --- 68030: empty the data cache (uses d0/a0) ---------------------------
; Run at the start of every tick, so what the handler reads from the
; cartridge window (the capture counter, the stream credit) comes from the
; RP, not from a line cached on an earlier tick. movec is assembled by hand:
; this file is built for the 68000, and only a 68030 gets here.
dcache_flush:
	lea		res_030(pc), a0
	tst.b	(a0)
	beq.s	.dcf_done
	dc.w	$4E7A,$0002				; movec cacr,d0
	bset	#CACR_CD, d0
	dc.w	$4E7B,$0002				; movec d0,cacr
.dcf_done:
	rts

; --- Send the BEGIN metadata command (rez + machine type + palette) -------
send_begin:
	move.w	#APP_SCREENSHOT_BEGIN, d0
	move.l	res_rez(pc), d3
	move.l	res_mch(pc), d4
	moveq	#0, d5
	move.l	res_palptr(pc), a4
	moveq	#0, d6
	move.w	res_palbytes(pc), d6	; 16 (ST) or 256 (TT) palette words
	bsr		call_transport
	rts

; --- Send one screen chunk (d1 = chunk index) ---------------------------
send_chunk:
	move.l	res_screenptr(pc), a4
	move.w	d1, d2
	mulu	res_chunk(pc), d2		; index * chunk size (32-bit result)
	add.l	d2, a4					; a4 = snapshot base + offset
	move.l	d1, d3					; d3 = chunk index param for the RP
	moveq	#0, d4
	moveq	#0, d5
	move.w	#APP_SCREENSHOT_DATA, d0
	moveq	#0, d6
	move.w	res_chunk(pc), d6
	bsr		call_transport
	rts

; --- Call the ROM write transport via absolute jsr, with retries --------
; Inputs already in d0/d3/d4/d5/d6/a4. The transport clobbers d1-d7/a0-a3 and
; advances a4, so the inputs are saved/restored around each attempt. The retry
; counter is kept on the stack (every data register is clobbered).
call_transport:
	move.l	#RES_RETRIES, -(sp)
.ct_retry:
	movem.l	d0/d3-d6/a4, -(sp)		; save the transport inputs
	bsr		cache_guard_begin
	movea.l	#send_sync_write_command_to_sidecart, a0
	jsr		(a0)
	bsr		cache_guard_end
	tst.w	d0						; d0 = 0 on success
	movem.l	(sp)+, d0/d3-d6/a4		; restore inputs (movem leaves CCR intact)
	beq.s	.ct_done				; success
	subq.l	#1, (sp)				; decrement retry counter on the stack
	bne.s	.ct_retry
.ct_done:
	addq.l	#4, sp					; pop the retry counter
	rts

; --- Cache guard around ROM3 side-effect reads -------------------------
; The SidecarTridge command protocol is encoded as reads from ROM3. On a
; Mega STE running 16 MHz + cache, those reads can be satisfied by cache and
; never reach the RP. Clear only bit0 of $FFFF8E21 while signalling a command,
; preserving the canonical MSTE_CC upper bits and restoring the caller's mode.
; A 68030's data cache would do the same, and would also serve the reply
; token the sync transport waits for from a stale line, so its data cache
; is emptied and turned off (CACR) for the duration. Uses d7/a0/a1.
cache_guard_begin:
	lea		res_cache_active(pc), a1
	clr.b	(a1)
	lea		res_030(pc), a1
	tst.b	(a1)
	beq.s	.cgb_mste
	dc.w	$4E7A,$7002				; movec cacr,d7
	lea		res_cacr(pc), a1
	move.l	d7, (a1)
	bclr	#CACR_ED, d7
	bset	#CACR_CD, d7
	dc.w	$4E7B,$7002				; movec d7,cacr
	rts
.cgb_mste:
	move.l	res_mch(pc), d7
	cmp.l	#MCH_MEGA_STE, d7
	bne.s	.cgb_done
	movea.l	#MEGASTE_CTRL_ADDR, a0
	move.b	(a0), d7
	lea		res_cache_saved(pc), a1
	move.b	d7, (a1)
	btst	#0, d7
	beq.s	.cgb_done
	bclr	#0, d7
	move.b	d7, (a0)
	lea		res_cache_active(pc), a1
	move.b	#1, (a1)
.cgb_done:
	rts

cache_guard_end:
	lea		res_030(pc), a1
	tst.b	(a1)
	beq.s	.cge_mste
	move.l	res_cacr(pc), d7
	dc.w	$4E7B,$7002				; movec d7,cacr (the caller's mode)
	rts
.cge_mste:
	lea		res_cache_active(pc), a1
	tst.b	(a1)
	beq.s	.cge_done
	movea.l	#MEGASTE_CTRL_ADDR, a0
	lea		res_cache_saved(pc), a1
	move.b	(a1), (a0)
	lea		res_cache_active(pc), a1
	clr.b	(a1)
.cge_done:
	rts

; --- Resident state (writable; lives in the RAM copy) -------------------
	even
res_state:
	dc.b	0						; 0 idle, 1 begin, 2..17 data chunk (state-2)
res_prev:
	dc.b	0						; last seen CAPTURE_SEQ value
	even
res_rez:
	dc.l	0						; captured resolution
res_mch:
	dc.l	0						; captured _MCH machine type
res_screenptr:
	dc.l	0						; -> snapshot buffer (set by installer)
res_palptr:
	dc.l	0						; -> palette buffer (set by installer)
orig_hook:
	dc.l	0						; saved original handler at the hooked vector
res_palbytes:						; set by snapshot, in this order:
	dc.w	0						; palette bytes sent with BEGIN
res_chunk:
	dc.w	0						; VBL chunk size
res_nchunks:
	dc.w	0						; VBL chunk count
res_frame:
	dc.w	0						; ETV frame size
res_nframes:
	dc.w	0						; ETV frame count
res_cache_saved:
	dc.b	0						; saved Mega STE $FFFF8E21 value
res_cache_active:
	dc.b	0						; non-zero when cache was disabled by guard
res_030:
	dc.b	0						; non-zero on a 68030 (set by installer)
	even
res_cacr:
	dc.l	0						; 68030 CACR saved by cache_guard_begin

; -----------------------------------------------------------------------
; ETV async stream entry -- installed at $400 only when HOOK_FLAG_ADDR is 1.
; It reuses the snapshot helpers and state above, but never calls the blocking
; synchronous transport. A frame is sent only when the RP-published credit in
; STREAM_ACK_ADDR advances.
; -----------------------------------------------------------------------
	even
resident_etv_start:
	movem.l	d0-d7/a0-a6, -(sp)
	lea		resident_etv_start(pc), a5
	bsr		dcache_flush

	move.b	etv_state(pc), d0
	beq.s	.etv_idle
	cmp.b	#1, d0
	beq.s	.etv_send_begin_state
	cmp.b	#2, d0
	beq.s	.etv_wait_begin_state
	cmp.b	#3, d0
	beq		.etv_send_frame_state
	cmp.b	#4, d0
	beq		.etv_wait_frame_state
	bra		.etv_abort

.etv_idle:
	move.b	CAPTURE_SEQ_ADDR, d0
	lea		res_prev(pc), a0
	cmp.b	(a0), d0
	beq		.etv_chain
	move.b	d0, (a0)
	moveq	#0, d1
	move.b	d0, d1
	move.w	d1, etv_seq-resident_etv_start(a5)
	bsr		snapshot
	clr.w	etv_frame-resident_etv_start(a5)
	clr.w	etv_retry-resident_etv_start(a5)
	clr.w	etv_timeout-resident_etv_start(a5)
	move.b	#1, etv_state-resident_etv_start(a5)
	bra		.etv_chain

.etv_send_begin_state:
	bsr		etv_send_begin
	clr.w	etv_retry-resident_etv_start(a5)
	move.b	#2, etv_state-resident_etv_start(a5)
	bra		.etv_chain

.etv_wait_begin_state:
	bsr		etv_ack_read
	tst.w	d0
	beq.s	.etv_wait_begin_retry
	tst.w	d1
	bne.s	.etv_wait_begin_retry
	clr.w	etv_frame-resident_etv_start(a5)
	clr.w	etv_retry-resident_etv_start(a5)
	clr.w	etv_timeout-resident_etv_start(a5)
	move.w	res_nframes(pc), d0
	beq		.etv_abort				; unsupported: the BEGIN was all
	bra.s	.etv_send_frame_state	; credit for frame 0: send it this tick
.etv_wait_begin_retry:
	bsr		etv_wait_tick
	tst.w	d0
	beq		.etv_chain
	move.b	#1, etv_state-resident_etv_start(a5)
	bra		.etv_chain

.etv_send_frame_state:
	bsr		etv_send_frame
	clr.w	etv_retry-resident_etv_start(a5)
	move.b	#4, etv_state-resident_etv_start(a5)
	bra		.etv_chain

.etv_wait_frame_state:
	bsr		etv_ack_read
	tst.w	d0
	beq.s	.etv_wait_frame_retry
	move.w	etv_frame(pc), d2
	addq.w	#1, d2
	cmp.w	d2, d1
	blo.s	.etv_wait_frame_retry
	move.w	d2, etv_frame-resident_etv_start(a5)
	clr.w	etv_retry-resident_etv_start(a5)
	clr.w	etv_timeout-resident_etv_start(a5)
	cmp.w	res_nframes(pc), d2
	bhs.s	.etv_abort
	bra.s	.etv_send_frame_state	; credit for the next frame: send it this
									; tick, a frame a tick rather than one
									; every other tick
.etv_wait_frame_retry:
	bsr		etv_wait_tick
	tst.w	d0
	beq		.etv_chain
	move.b	#3, etv_state-resident_etv_start(a5)
	bra		.etv_chain

.etv_abort:
	clr.b	etv_state-resident_etv_start(a5)
	clr.w	etv_retry-resident_etv_start(a5)
	clr.w	etv_timeout-resident_etv_start(a5)

.etv_chain:
	movem.l	(sp)+, d0-d7/a0-a6
	move.l	orig_hook(pc), -(sp)
	rts

; Return d0=1 and d1=next frame when STREAM_ACK_ADDR matches etv_seq.
; Return d0=0 on no usable ack.
etv_ack_read:
	move.l	STREAM_ACK_ADDR, d0
	move.w	d0, d1					; d1 = next frame index
	swap	d0						; d0.w = ack sequence
	cmp.w	etv_seq(pc), d0
	bne.s	.ear_no
	moveq	#1, d0
	rts
.ear_no:
	moveq	#0, d0
	rts

; Tick retry/abort counters. Return d0=1 when caller should resend, d0=0 to
; keep waiting. If the abort limit expires the ETV state is cleared here.
etv_wait_tick:
	lea		etv_timeout(pc), a0
	addq.w	#1, (a0)
	cmp.w	#ETV_ABORT_TICKS, (a0)
	blo.s	.ewt_retry
	clr.b	etv_state-resident_etv_start(a5)
	moveq	#0, d0
	rts
.ewt_retry:
	lea		etv_retry(pc), a0
	addq.w	#1, (a0)
	cmp.w	#ETV_RETRY_TICKS, (a0)
	blo.s	.ewt_wait
	clr.w	(a0)
	moveq	#1, d0
	rts
.ewt_wait:
	moveq	#0, d0
	rts

etv_send_begin:
	move.w	#APP_SCREENSHOT_STREAM_BEGIN, d0
	moveq	#0, d3
	move.w	etv_seq(pc), d3
	move.l	res_rez(pc), d4
	move.l	res_mch(pc), d5
	move.l	res_palptr(pc), a4
	moveq	#0, d6
	move.w	res_palbytes(pc), d6
	bsr		etv_async_write
	rts

etv_send_frame:
	moveq	#0, d1
	move.w	etv_frame(pc), d1
	move.l	res_screenptr(pc), a4
	move.w	d1, d2
	mulu	res_frame(pc), d2
	add.l	d2, a4
	move.w	#APP_SCREENSHOT_STREAM_DATA, d0
	moveq	#0, d3
	move.w	etv_seq(pc), d3
	moveq	#0, d4
	move.w	d1, d4
	moveq	#0, d5
	moveq	#0, d6
	move.w	res_frame(pc), d6
	bsr		etv_async_write
	rts

; Async ROM3 write command. Inputs:
; d0.w command, d3/d4/d5 long params, d6.w byte count, a4 even buffer pointer.
; Emits the same protocol as send_sync_write_command_to_sidecart but skips the
; random-token wait.
etv_async_write:
	bsr		cache_guard_begin
	move.l	RANDOM_TOKEN_SEED_ADDR, d2
	movea.l	#ROMCMD_START_ADDR, a0
	adda.l	#$8000, a0

	move.w	#CMD_MAGIC_NUMBER, d7
	tst.b	(a0, d7.w)
	clr.l	d7

	add.w	d0, d7
	tst.b	(a0, d0.w)

	move.l	d6, d1
	add.l	#16, d1
	addq.l	#1, d1
	lsr.l	#1, d1
	lsl.l	#1, d1
	add.w	d1, d7
	tst.b	(a0, d1.w)

	add.w	d2, d7
	tst.b	(a0, d2.w)
	swap	d2
	add.w	d2, d7
	tst.b	(a0, d2.w)

	add.w	d3, d7
	tst.b	(a0, d3.w)
	swap	d3
	add.w	d3, d7
	tst.b	(a0, d3.w)

	add.w	d4, d7
	tst.b	(a0, d4.w)
	swap	d4
	add.w	d4, d7
	tst.b	(a0, d4.w)

	add.w	d5, d7
	tst.b	(a0, d5.w)
	swap	d5
	add.w	d5, d7
	tst.b	(a0, d5.w)

	move.w	d6, d1
	lsr.w	#1, d1
	subq.w	#1, d1
.eaw_buf:
	move.w	(a4)+, d2
	add.w	d2, d7
	tst.b	(a0, d2.w)
	dbf		d1, .eaw_buf

	tst.b	(a0, d7.w)
	bsr		cache_guard_end
	rts

	even
etv_state:
	dc.b	0						; 0 idle, 1 send begin, 2 wait begin,
									; 3 send frame, 4 wait frame
	even
etv_seq:
	dc.w	0
etv_frame:
	dc.w	0
etv_retry:
	dc.w	0
etv_timeout:
	dc.w	0
resident_end:
	even
