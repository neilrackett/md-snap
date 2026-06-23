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
VBL_VECTOR			equ $70			; .l level-4 (VBL) autovector
ETV_TIMER_VECTOR	equ $400		; .l etv_timer subroutine called by TOS
_p_cookies			equ $5a0		; .l pointer to the system cookie jar (0 = none)

; Hardware registers (long-absolute; the 68000 truncates to $FF82xx).
SHIFTER_REZ			equ $FFFF8260	; resolution: 0=low, 1=med, 2=high (low 2 bits)
PALETTE				equ $FFFF8240	; 16 colour words
VID_BASE_HI			equ $FFFF8201	; video base address, bits 16-23
VID_BASE_MID		equ $FFFF8203	; video base address, bits 8-15
									; bits 0-7 ($FF820D) are STE-only and 0 for
									; normal desktop output, so we assume 0.
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
RES_RETRIES			equ 3			; transport retries per chunk
ETV_RETRY_TICKS	equ 20			; 20 * 5 ms = about 100 ms
ETV_ABORT_TICKS	equ 800			; 800 * 5 ms = about 4 seconds

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
	cmp.b	#(SHOT_NUM_CHUNKS+2), d0	; 1 (begin) + 16 (data) done after state 17
	bne.s	.store_state
	moveq	#0, d0					; finished -> back to idle
.store_state:
	move.b	d0, (a0)

.chain:
	movem.l	(sp)+, d0-d7/a0-a6
	move.l	orig_hook(pc), -(sp)	; chain into the saved original handler...
	rts								; VBL original rte's, popping the interrupt frame

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

; --- Mega STE cache guard around ROM3 side-effect reads -----------------
; The SidecarTridge command protocol is encoded as reads from ROM3. On a
; Mega STE running 16 MHz + cache, those reads can be satisfied by cache and
; never reach the RP. Clear only bit0 of $FFFF8E21 while signalling a command,
; preserving the canonical MSTE_CC upper bits and restoring the caller's mode.
cache_guard_begin:
	lea		res_cache_active(pc), a1
	clr.b	(a1)
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
	dc.l	0						; -> 32000-byte snapshot buffer (set by installer)
orig_hook:
	dc.l	0						; saved original handler at the hooked vector
	even
res_pal:
	dcb.w	16, 0					; captured 16-colour palette (32 bytes)
res_cache_saved:
	dc.b	0						; saved Mega STE $FFFF8E21 value
res_cache_active:
	dc.b	0						; non-zero when cache was disabled by guard

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
	move.b	#3, etv_state-resident_etv_start(a5)
	bra		.etv_chain
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
	cmp.w	#SHOT_STREAM_FRAME_COUNT, d2
	bhs.s	.etv_abort
	move.b	#3, etv_state-resident_etv_start(a5)
	bra		.etv_chain
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
	lea		res_pal(pc), a4
	move.l	#32, d6
	bsr		etv_async_write
	rts

etv_send_frame:
	moveq	#0, d1
	move.w	etv_frame(pc), d1
	move.l	res_screenptr(pc), a4
	move.w	d1, d2
	mulu	#SHOT_STREAM_FRAME_SIZE, d2
	add.l	d2, a4
	move.w	#APP_SCREENSHOT_STREAM_DATA, d0
	moveq	#0, d3
	move.w	etv_seq(pc), d3
	moveq	#0, d4
	move.w	d1, d4
	moveq	#0, d5
	move.l	#SHOT_STREAM_FRAME_SIZE, d6
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
