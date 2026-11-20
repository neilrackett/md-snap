; Copyright (C) 2026 Neil Rackett
; SPDX-License-Identifier: GPL-3.0-or-later

; MD/Snap's menu on a TT, at cartridge offset $1800 (TTMENU in main.s).
;
; With a colour monitor the menu is shown in TT low (320x480, 256 colours)
; rather than ST low, so a screenshot's preview can show all of its colours:
; the RP's 320x200 menu is drawn with every line doubled, 320x400 centred in
; the 480 lines, the preview is 144x180 with its own 256-colour palette, and
; palette entries 0 and 255 are the menu's paper and ink. With a TT high
; (mono) monitor the menu is drawn four times over, 1280x800 centred in
; 1280x960, and there is no preview.
;
; Runs from ROM: it writes only to the screen and the hardware. Position
; independent, reached by an absolute jsr through the jump table below.

	section text

; Shared region (must match main.s and rp/src/preview_overlay.c).
ROM4_ADDR			equ $FA0000
FRAMEBUFFER_ADDR	equ (ROM4_ADDR + $10000 - 8000)	; the RP's 320x200 menu
APP_BUFFERS_ADDR	equ (ROM4_ADDR + $2100)
TRANSTABLE			equ APP_BUFFERS_ADDR			; byte -> pixel-doubled word
PREVIEW_FLAG		equ (APP_BUFFERS_ADDR + $200)	; non-zero: a preview to show
TT_PREVIEW_PALETTE	equ (PREVIEW_FLAG + 2)			; 256 TT colour words
TT_PREVIEW_DATA		equ (PREVIEW_FLAG + 2 + 512)	; 144x180, 8 interleaved planes
DISPLAY_BYPASS_FRAMEBUFFER	equ 1					; as main.s: swap framebuffer bytes

; Hardware and TOS.
_p_cookies			equ $5a0
TT_SHIFTER_MODE		equ $FFFF8262	; mode in bits 8-10
TT_PALETTE			equ $FFFF8400	; 256 colour words, 0RGB
XBIOS_SETSCREEN		equ 5
XBIOS_GETREZ		equ 4
REZ_ST_HIGH			equ 2
REZ_TT_HIGH			equ 6
REZ_TT_LOW			equ 7

; Layout. The menu is 200 lines of 40 bytes; a TT low line is 320 bytes and a
; TT high one 160, and either way the 40 or 80 lines above and below the menu
; are 12800 bytes each.
MENU_ROWS			equ 200
MENU_ROW_WORDS		equ 20
TT_LOW_ROW			equ 320
TT_HIGH_ROW			equ 160
TT_SCREEN_SIZE		equ 153600
BORDER_SIZE			equ 12800
PREVIEW_ROWS		equ 180
PREVIEW_ROW_BYTES	equ 144			; 144 pixels: 9 groups of 8 plane words
PREVIEW_OFFSET		equ (BORDER_SIZE + (64 * TT_LOW_ROW) + (11 * 16))	; menu x=176, y=32

ttmenu:
	bra.w	ttmenu_rez				; TTMENU_REZ
	bra.w	ttmenu_draw				; TTMENU_DRAW

; --- Choose the menu's mode ---------------------------------------------
; On a TT: d0 = 2 (ST high) or 6 (TT high), left as they are, or 7 (TT low),
; switched to from any other mode with Setscreen. Anything else: d0 = -1.
; Supervisor (reads the cookie jar); uses d0-d2/a0-a2 like XBIOS.
ttmenu_rez:
	move.l	_p_cookies.w, d0
	beq.s	.not_tt
	movea.l	d0, a0
.find_vdo:
	move.l	(a0)+, d0
	beq.s	.not_tt					; end of the jar
	cmp.l	#'_VDO', d0
	beq.s	.found_vdo
	addq.l	#4, a0
	bra.s	.find_vdo
.found_vdo:
	cmp.w	#2, (a0)				; $0002xxxx: a TT's video
	bne.s	.not_tt
	move.w	#XBIOS_GETREZ, -(sp)
	trap	#14
	addq.l	#2, sp
	cmp.w	#REZ_ST_HIGH, d0
	beq.s	.rez_done
	cmp.w	#REZ_TT_HIGH, d0
	beq.s	.rez_done
	cmp.w	#REZ_TT_LOW, d0
	beq.s	.rez_done
	move.w	#REZ_TT_LOW, -(sp)		; Setscreen(-1, -1, TT low)
	move.l	#-1, -(sp)
	move.l	#-1, -(sp)
	move.w	#XBIOS_SETSCREEN, -(sp)
	trap	#14
	lea		12(sp), sp
	moveq	#REZ_TT_LOW, d0
.rez_done:
	rts
.not_tt:
	moveq	#-1, d0
	rts

; --- Draw the menu ------------------------------------------------------
; a0 = screen base, in TT low or TT high. Uses d0-d6/a0-a3.
ttmenu_draw:
	bsr		clear_borders
	move.w	TT_SHIFTER_MODE, d0
	lsr.w	#8, d0
	and.w	#7, d0
	cmp.w	#REZ_TT_HIGH, d0
	beq		draw_high
	cmp.w	#REZ_TT_LOW, d0
	beq.s	draw_low
	rts

; The lines above and below the menu: paper, whatever TOS left there.
clear_borders:
	movea.l	a0, a1
	bsr.s	.clear
	movea.l	a0, a1
	adda.l	#(TT_SCREEN_SIZE - BORDER_SIZE), a1
.clear:
	moveq	#0, d0
	move.w	#(BORDER_SIZE / 4 - 1), d1
.clear_long:
	move.l	d0, (a1)+
	dbf		d1, .clear_long
	rts

; TT low: each menu word goes into all 8 planes (paper 0, ink 255), and each
; line twice.
draw_low:
	lea		BORDER_SIZE(a0), a1
	movea.l	#FRAMEBUFFER_ADDR, a2
	move.w	#(MENU_ROWS - 1), d2
.row:
	lea		TT_LOW_ROW(a1), a3		; the line below: the same again
	moveq	#(MENU_ROW_WORDS - 1), d1
.word:
	move.w	(a2)+, d0
	ifne DISPLAY_BYPASS_FRAMEBUFFER == 1
	rol.w	#8, d0
	endif
	move.w	d0, d3
	swap	d3
	move.w	d0, d3					; the word twice: two planes a long
	move.l	d3, (a1)+
	move.l	d3, (a1)+
	move.l	d3, (a1)+
	move.l	d3, (a1)+
	move.l	d3, (a3)+
	move.l	d3, (a3)+
	move.l	d3, (a3)+
	move.l	d3, (a3)+
	dbf		d1, .word
	lea		TT_LOW_ROW(a1), a1		; past the line drawn twice
	dbf		d2, .row

	tst.w	PREVIEW_FLAG
	beq.s	.paper_and_ink

	; The preview, 144x180 at the menu's x=176, y=32.
	movea.l	#TT_PREVIEW_DATA, a2
	movea.l	a0, a1
	adda.l	#PREVIEW_OFFSET, a1		; past a 16-bit displacement
	move.w	#(PREVIEW_ROWS - 1), d2
.preview_row:
	moveq	#(PREVIEW_ROW_BYTES / 4 - 1), d1
.preview_long:
	move.l	(a2)+, (a1)+
	dbf		d1, .preview_long
	lea		(TT_LOW_ROW - PREVIEW_ROW_BYTES)(a1), a1
	dbf		d2, .preview_row

	; Its palette, entries 0 and 255 included (the RP keeps them white and
	; black).
	movea.l	#TT_PREVIEW_PALETTE, a2
	movea.l	#TT_PALETTE, a1
	move.w	#255, d1
.palette:
	move.w	(a2)+, (a1)+
	dbf		d1, .palette
	rts

.paper_and_ink:
	move.w	#$0FFF, TT_PALETTE			; 0: white paper
	move.w	#$0000, TT_PALETTE + 510	; 255: black ink
	rts

; TT high: each menu byte becomes a long (every pixel four wide, by the
; pixel-doubling table twice) and each line four lines. Paper is white and
; ink black on a TT high monitor, whatever the palette says.
draw_high:
	lea		BORDER_SIZE(a0), a1
	movea.l	#FRAMEBUFFER_ADDR, a2
	movea.l	#TRANSTABLE, a3
	move.w	#(MENU_ROWS - 1), d2
.hrow:
	moveq	#(MENU_ROW_WORDS - 1), d1
.hword:
	move.w	(a2)+, d0
	ifne DISPLAY_BYPASS_FRAMEBUFFER == 1
	rol.w	#8, d0
	endif
	move.w	d0, d3
	lsr.w	#8, d3					; the left byte
	bsr.s	expand4
	bsr.s	put4
	move.w	d0, d3
	and.w	#$FF, d3				; the right byte
	bsr.s	expand4
	bsr.s	put4
	dbf		d1, .hword
	lea		(3 * TT_HIGH_ROW)(a1), a1	; past the four lines
	dbf		d2, .hrow
	rts

; d3 = a byte (0-255) -> d4 = its pixels, each four wide. Uses d5/d6.
expand4:
	add.w	d3, d3
	move.w	(a3, d3.w), d5			; each pixel two wide
	move.w	d5, d6
	lsr.w	#8, d6
	add.w	d6, d6
	move.w	(a3, d6.w), d4
	swap	d4
	and.w	#$FF, d5
	add.w	d5, d5
	move.w	(a3, d5.w), d4
	rts

; d4 into four lines at a1, then a1 to the next long.
put4:
	move.l	d4, (a1)
	move.l	d4, TT_HIGH_ROW(a1)
	move.l	d4, (2 * TT_HIGH_ROW)(a1)
	move.l	d4, (3 * TT_HIGH_ROW)(a1)
	addq.l	#4, a1
	rts
