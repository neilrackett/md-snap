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
; SELECT short-press. A handler hooked onto the VBL autovector ($70) polls that
; byte every frame; when it changes it snapshots the screen + palette + rez +
; machine type into a RAM buffer, then dribbles the 32000-byte screen out the
; ROM3 window to the RP, one 2000-byte chunk per frame (16 chunks + 1 metadata
; command). The RP deplanes, pixel-doubles and writes an indexed PNG.
;
; Why hook $70 and not _vblqueue
; ------------------------------
; A free _vblqueue slot works during boot but GEM reclaims it when the desktop
; loads, silently de-linking us. The $70 autovector is the OS's VBL entry point
; and GEM does not replace it, so a hook there survives into the desktop. We
; chain to the saved original so the OS's VBL processing still runs.
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
VBL_VECTOR			equ $70			; .l level-4 (VBL) autovector
_p_cookies			equ $5a0		; .l pointer to the system cookie jar (0 = none)

; Hardware registers (long-absolute; the 68000 truncates to $FF82xx).
SHIFTER_REZ			equ $FFFF8260	; resolution: 0=low, 1=med, 2=high (low 2 bits)
PALETTE				equ $FFFF8240	; 16 colour words
VID_BASE_HI			equ $FFFF8201	; video base address, bits 16-23
VID_BASE_MID		equ $FFFF8203	; video base address, bits 8-15
									; bits 0-7 ($FF820D) are STE-only and 0 for
									; normal desktop output, so we assume 0.

; Shared region / protocol (must match main.s).
CAPTURE_SEQ_ADDR	equ $FA201F		; LSB of shared-var slot 3 (RP bumps it)
APP_SCREENSHOT_BEGIN	equ $10
APP_SCREENSHOT_DATA		equ $11
SHOT_CHUNK_SIZE		equ 2000
SHOT_NUM_CHUNKS		equ 16
SHOT_SCREEN_SIZE	equ 32000
RES_RETRIES			equ 3			; transport retries per chunk

; The write transport lives in main.s (ROM). Imported so the resident RAM copy
; can call it via an absolute-address jsr.
	xref send_sync_write_command_to_sidecart

; -----------------------------------------------------------------------
; Installer -- runs from ROM at USERFW ($FA0800). Only writes to RAM.
; GEMDOS/XBIOS preserve d3-d7/a3-a6, so a3 (the block base) survives the traps.
; -----------------------------------------------------------------------
userfw:
	movem.l	d0-d2/a0-a3, -(sp)

	; Malloc the resident block + the trailing screen snapshot buffer.
	move.l	#((resident_end-resident_start)+SHOT_SCREEN_SIZE), -(sp)
	move.w	#GEMDOS_Malloc, -(sp)
	trap	#1
	addq.l	#6, sp
	tst.l	d0
	beq.s	.uf_exit				; out of memory -> install nothing
	movea.l	d0, a3					; a3 = RAM block base (survives traps)

	; Copy the resident code+state (NOT the trailing snapshot buffer) into RAM.
	lea		resident_start(pc), a0
	movea.l	a3, a1
	move.w	#(resident_end-resident_start-1), d1
.uf_copy:
	move.b	(a0)+, (a1)+
	dbf		d1, .uf_copy

	; The snapshot buffer sits right after the copied block; store its address
	; into the block so the resident code can find it.
	move.l	a3, d0
	add.l	#(resident_end-resident_start), d0
	move.l	d0, (res_screenptr-resident_start)(a3)

	; Start idle, and seed res_prev with the current counter so a stale value
	; can't fire a capture the instant we install.
	clr.b	(res_state-resident_start)(a3)
	move.b	CAPTURE_SEQ_ADDR, (res_prev-resident_start)(a3)

	; Hook $70: save the original into the block, point $70 at our handler
	; (block offset 0). Mask interrupts across the swap. $70 is supervisor-only;
	; the installer runs supervisor at boot, so the access is legal.
	move.w	sr, -(sp)
	ori.w	#$0700, sr
	move.l	VBL_VECTOR.w, (orig_vbl-resident_start)(a3)
	move.l	a3, VBL_VECTOR.w
	move.w	(sp)+, sr

.uf_exit:
	movem.l	(sp)+, d0-d2/a0-a3
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
	move.b	#1, (a0)				; next frame starts sending (state 1 = BEGIN)
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
	cmp.b	#(SHOT_NUM_CHUNKS+2), d0	; 1 (begin) + 16 (data) done after state 17
	bne.s	.store_state
	moveq	#0, d0					; finished -> back to idle
.store_state:
	move.b	d0, (a0)

.chain:
	movem.l	(sp)+, d0-d7/a0-a6
	move.l	orig_vbl(pc), -(sp)		; chain into the original VBL handler...
	rts								; ...which rte's, popping the interrupt frame

; --- Snapshot the current screen state into the RAM block ---------------
snapshot:
	; Resolution (low 2 bits of the Shifter resolution register).
	moveq	#0, d0
	move.b	SHIFTER_REZ, d0
	and.w	#3, d0
	lea		res_rez(pc), a0
	move.l	d0, (a0)

	; Machine type from the _MCH cookie (high word: 0=ST, 1=STE, 2=Mega STE...).
	bsr		read_mch				; -> d0
	lea		res_mch(pc), a0
	move.l	d0, (a0)

	; Palette: 16 words.
	movea.l	#PALETTE, a0
	lea		res_pal(pc), a1
	moveq	#15, d0
.snap_pal:
	move.w	(a0)+, (a1)+
	dbf		d0, .snap_pal

	; Screen base = HI<<16 | MID<<8 (low byte assumed 0).
	moveq	#0, d0
	move.b	VID_BASE_HI, d0
	lsl.l	#8, d0
	move.b	VID_BASE_MID, d0
	lsl.l	#8, d0
	movea.l	d0, a0					; a0 = screen base
	move.l	res_screenptr(pc), a1	; a1 = snapshot buffer
	move.w	#((SHOT_SCREEN_SIZE/4)-1), d0
.snap_copy:
	move.l	(a0)+, (a1)+
	dbf		d0, .snap_copy			; 8000 longs = 32000 bytes
	rts

; --- Read the _MCH cookie value into d0 (0 = assume plain ST) -----------
read_mch:
	moveq	#0, d0
	move.l	_p_cookies.w, d1
	beq.s	.mch_done
	movea.l	d1, a0
.mch_loop:
	move.l	(a0)+, d1
	beq.s	.mch_done				; 0 = end of jar
	cmp.l	#'_MCH', d1
	beq.s	.mch_found
	addq.l	#4, a0					; skip the value, try next cookie
	bra.s	.mch_loop
.mch_found:
	move.l	(a0), d0				; _MCH value
.mch_done:
	rts

; --- Send the BEGIN metadata command (rez + machine type + palette) -------
send_begin:
	move.w	#APP_SCREENSHOT_BEGIN, d0
	move.l	res_rez(pc), d3
	move.l	res_mch(pc), d4
	moveq	#0, d5
	lea		res_pal(pc), a4
	move.l	#32, d6					; 16 palette words
	bsr		call_transport
	rts

; --- Send one screen chunk (d1 = chunk index) ---------------------------
send_chunk:
	move.l	res_screenptr(pc), a4
	move.w	d1, d2
	mulu	#SHOT_CHUNK_SIZE, d2	; index * 2000 (<= 30000)
	add.l	d2, a4					; a4 = snapshot base + offset
	move.l	d1, d3					; d3 = chunk index param for the RP
	moveq	#0, d4
	moveq	#0, d5
	move.w	#APP_SCREENSHOT_DATA, d0
	move.l	#SHOT_CHUNK_SIZE, d6
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
	movea.l	#send_sync_write_command_to_sidecart, a0
	jsr		(a0)
	tst.w	d0						; d0 = 0 on success
	movem.l	(sp)+, d0/d3-d6/a4		; restore inputs (movem leaves CCR intact)
	beq.s	.ct_done				; success
	subq.l	#1, (sp)				; decrement retry counter on the stack
	bne.s	.ct_retry
.ct_done:
	addq.l	#4, sp					; pop the retry counter
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
	dc.l	0						; -> 32000-byte snapshot buffer (set by installer)
orig_vbl:
	dc.l	0						; saved original $70 handler
	even
res_pal:
	dcb.w	16, 0					; captured 16-colour palette (32 bytes)
resident_end:
	even
