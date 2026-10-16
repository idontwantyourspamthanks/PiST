; ---------------------------------------------------------------------------
; A PiST bitplane export that shows itself off: "Phase 1" scrolls across an ST low-resolution screen,
; animating through its 6 frame(s), one step per VBL, until you press a key.
;
; Assemble it in PiST with F7, or by hand:
;     vasmm68k_mot -Ftos -o scroll.prg scroll.s
;
; It pulls its data out of the .dat the export wrote beside it, so
; keep the two files together (vasm looks for an incbin next to the
; source it is assembling). Every block in that file is named by an
; `equ` down there; the ones this program draws with are:
;     palette                 $0000  32
;     sprite_masked_shift0_f0 $1CA0  960
;     sprite_masked_shift1_f0 $2060  960
;     sprite_masked_shift2_f0 $2420  960
;     sprite_masked_shift3_f0 $27E0  960
;     sprite_masked_shift4_f0 $2BA0  960
;     sprite_masked_shift5_f0 $2F60  960
;     sprite_masked_shift6_f0 $3320  960
;     sprite_masked_shift7_f0 $36E0  960
; Each frame is the same shape, 14976 bytes further on: frame f of a block is `base + f*kFrameStride`.
;
; A 16-pixel group is four plane words (and a mask word in front of them
; where the data is masked). The blit ANDs the mask into the screen and ORs
; the planes over it, which is why a set mask bit keeps what was there.
; The mask leaves out palette colour 0, on top of the pixels that are not painted:
; the blit keeps the screen under both.
; The drawing is driven by sprite_masked_shift0_f0: 8 copies, 2 pixels apart.
;
; It wants a colour monitor (RGB/VGA/TV), which is what the ST's 320x200
; four-bitplane screen needs; on any other screen it says so and waits for a
; key rather than drawing.
;
; Supervisor mode is not optional: on the ST the GLUE bus-errors user-mode
; access to the system area ($0-$7ff) and to the hardware registers, so the
; palette and the screen base can only be reached after GEMDOS Super(0).
; ---------------------------------------------------------------------------

kScreenRowBytes	equ	160		; one screen row: 320/16 groups of four plane words
kScreenBytes	equ	32000		; the whole screen, cleared at startup
kSpriteGroups	equ	3		; 16-pixel groups in one data row
kSpriteRowBytes	equ	30		; bytes in one data row: four planes and a mask word
kDrawGroups	equ	3		; groups the blit writes per row
kSpriteWidth	equ	32		; painted pixels
kSpriteHeight	equ	32
kShifts		equ	8		; pre-shifted copies in the data
kCopyStride	equ	960		; bytes from one copy to the next
kFrames		equ	6		; animation frames in this phase
kFrameStride	equ	14976		; bytes from one frame to the next
kFrameTicks	equ	4		; VBLs each frame is shown for
kStep		equ	2		; pixels the sprite moves per frame
kMaxX		equ	272		; last x that still fits in the screen row
kSpriteY	equ	84		; the row it travels along

start:
	clr.l	-(sp)			; GEMDOS Super(0): into supervisor mode
	move.w	#$20,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d0,saved_ssp		; the stack to go back to on the way out

	lea	$ffff8240,a0		; keep the colours we found...
	lea	saved_palette,a1
	movem.l	(a0),d0-d7
	movem.l	d0-d7,(a1)
	move.w	#4,-(sp)			; XBIOS Getrez
	trap	#14
	addq.l	#2,sp
	tst.w	d0
	beq	.low_res
	move.l	#msg_res,-(sp)		; not 320x200 with 4 planes: say so, and
	move.w	#9,-(sp)			;   wait so the message can be read
	trap	#1
	addq.l	#6,sp
	move.w	#7,-(sp)			; GEMDOS Crawcin
	trap	#1
	addq.l	#2,sp
	bra	quit
.low_res:
	lea	palette,a1		; ...and install ours. a0 is reloaded: the
	lea	$ffff8240,a0		;   traps in between clobber it
	movem.l	(a1),d0-d7
	movem.l	d0-d7,(a0)
	move.w	#3,-(sp)			; XBIOS Logbase: the screen TOS draws to
	trap	#14
	addq.l	#2,sp
	movea.l	d0,a6

	move.l	a6,a1			; start from a clean screen
	move.w	#(kScreenBytes/4)-1,d0
.clear:
	clr.l	(a1)+
	dbf	d0,.clear

.drain:				; a key already waiting must not end this
	move.w	#$0b,-(sp)		;   on the first frame. GEMDOS Cconis,
	trap	#1			;   not Crawio (6): handed no character
	addq.l	#2,sp			;   argument it still answers non-zero
	tst.w	d0
	bne	.drain

	moveq	#0,d4			; d4 = x to draw at
	moveq	#0,d5			; d5 = x already on the screen
	clr.w	anim_frame		; start on the first frame
	move.w	#kFrameTicks,anim_ticks
.frame:
	move.w	#37,-(sp)			; XBIOS Vsync: one step per frame
	trap	#14
	addq.l	#2,sp
	move.w	d5,d0			; rub out where it was...
	bsr	erase_at
	move.w	d4,d0			; ...and draw where it is now
	bsr	blit_at
	move.w	d4,d5
	add.w	#kStep,d4
	cmpi.w	#kMaxX+1,d4
	blt	.no_wrap
	moveq	#0,d4
.no_wrap:
	subq.w	#1,anim_ticks		; animate: hold each frame for a few
	bgt	.same_frame		;   VBLs, then move to the next
	move.w	#kFrameTicks,anim_ticks
	move.w	anim_frame,d2
	addq.w	#1,d2
	cmpi.w	#kFrames,d2
	blt	.store_frame
	moveq	#0,d2
.store_frame:
	move.w	d2,anim_frame
.same_frame:
	move.w	#$0b,-(sp)		; GEMDOS Cconis: a key means stop
	trap	#1
	addq.l	#2,sp
	tst.w	d0
	beq	.frame

	lea	saved_palette,a1		; put the colours back
	movem.l	(a1),d0-d7
	lea	$ffff8240,a0
	movem.l	d0-d7,(a0)
quit:
	move.l	saved_ssp,-(sp)		; Super(old_ssp): back to user mode
	move.w	#$20,-(sp)
	trap	#1
	addq.l	#6,sp
	clr.w	-(sp)			; Pterm0
	trap	#1

; d0.w = x: draw the current animation frame with its left edge there. One
; data row per screen row, one group at a time, starting x/16 words into the
; row. Registers: a0 = data, a1 = screen, a6 = screen base, d1-d3, d6, d7
; scratch.
blit_at:
	moveq	#0,d1			; move.w leaves the top half alone, and
	move.w	d0,d1			;   the address below is 32 bits wide
	lsr.w	#4,d1			; x/16 whole groups in
	lsl.w	#3,d1			; a group is four plane words: eight bytes
	add.l	#kSpriteY*kScreenRowBytes,d1
	move.l	a6,a1
	adda.l	d1,a1			; a1 = the sprite's first screen word
	moveq	#0,d2
	move.w	anim_frame,d2		; which animation frame...
	mulu	#kFrameStride,d2
	move.w	d0,d3			; ...and which pre-shifted copy: the
	andi.w	#15,d3			;   pixel offset inside the group, one
	lsr.w	#1,d3
	mulu	#kCopyStride,d3			;   per kStep pixels
	add.l	d3,d2
	lea	sprite_masked_shift0_f0,a0
	adda.l	d2,a0
	move.w	#kSpriteHeight-1,d7
.row:
	move.w	#kDrawGroups-1,d6
.group:
	move.w	(a0)+,d3			; the mask, kept for all four planes
	move.w	(a0)+,d1
	and.w	d3,(a1)
	or.w	d1,(a1)+
	move.w	(a0)+,d1
	and.w	d3,(a1)
	or.w	d1,(a1)+
	move.w	(a0)+,d1
	and.w	d3,(a1)
	or.w	d1,(a1)+
	move.w	(a0)+,d1
	and.w	d3,(a1)
	or.w	d1,(a1)+
	dbf	d6,.group
	adda.l	#(kScreenRowBytes-kDrawGroups*8),a1
	dbf	d7,.row
	rts

; d0.w = x: clear the sprite's rectangle, so the next frame does not
; leave a trail. The background is colour 0, which is what it clears to.
erase_at:
	moveq	#0,d1			; as in blit_at: clear the whole register
	move.w	d0,d1			;   before the word arithmetic
	lsr.w	#4,d1
	lsl.w	#3,d1
	add.l	#kSpriteY*kScreenRowBytes,d1
	move.l	a6,a1
	adda.l	d1,a1
	move.w	#kSpriteHeight-1,d7
.row:
	move.w	#kDrawGroups-1,d6
.group:
	clr.l	(a1)+
	clr.l	(a1)+
	dbf	d6,.group
	adda.l	#(kScreenRowBytes-kDrawGroups*8),a1
	dbf	d7,.row
	rts

; ---------------------------------------------------------------------------
; The data: the .dat this export wrote, as it was written.
; ---------------------------------------------------------------------------
	even
bitplanes:
	incbin	"sprites.dat"
	even

palette                 equ	bitplanes+$0000
sprite_f0               equ	bitplanes+$0020
sprite_masked_f0        equ	bitplanes+$0220
sprite_shift0_f0        equ	bitplanes+$04A0
sprite_shift1_f0        equ	bitplanes+$07A0
sprite_shift2_f0        equ	bitplanes+$0AA0
sprite_shift3_f0        equ	bitplanes+$0DA0
sprite_shift4_f0        equ	bitplanes+$10A0
sprite_shift5_f0        equ	bitplanes+$13A0
sprite_shift6_f0        equ	bitplanes+$16A0
sprite_shift7_f0        equ	bitplanes+$19A0
sprite_masked_shift0_f0 equ	bitplanes+$1CA0
sprite_masked_shift1_f0 equ	bitplanes+$2060
sprite_masked_shift2_f0 equ	bitplanes+$2420
sprite_masked_shift3_f0 equ	bitplanes+$27E0
sprite_masked_shift4_f0 equ	bitplanes+$2BA0
sprite_masked_shift5_f0 equ	bitplanes+$2F60
sprite_masked_shift6_f0 equ	bitplanes+$3320
sprite_masked_shift7_f0 equ	bitplanes+$36E0
sprite_f1               equ	bitplanes+$3AA0
sprite_masked_f1        equ	bitplanes+$3CA0
sprite_shift0_f1        equ	bitplanes+$3F20
sprite_shift1_f1        equ	bitplanes+$4220
sprite_shift2_f1        equ	bitplanes+$4520
sprite_shift3_f1        equ	bitplanes+$4820
sprite_shift4_f1        equ	bitplanes+$4B20
sprite_shift5_f1        equ	bitplanes+$4E20
sprite_shift6_f1        equ	bitplanes+$5120
sprite_shift7_f1        equ	bitplanes+$5420
sprite_masked_shift0_f1 equ	bitplanes+$5720
sprite_masked_shift1_f1 equ	bitplanes+$5AE0
sprite_masked_shift2_f1 equ	bitplanes+$5EA0
sprite_masked_shift3_f1 equ	bitplanes+$6260
sprite_masked_shift4_f1 equ	bitplanes+$6620
sprite_masked_shift5_f1 equ	bitplanes+$69E0
sprite_masked_shift6_f1 equ	bitplanes+$6DA0
sprite_masked_shift7_f1 equ	bitplanes+$7160
sprite_f2               equ	bitplanes+$7520
sprite_masked_f2        equ	bitplanes+$7720
sprite_shift0_f2        equ	bitplanes+$79A0
sprite_shift1_f2        equ	bitplanes+$7CA0
sprite_shift2_f2        equ	bitplanes+$7FA0
sprite_shift3_f2        equ	bitplanes+$82A0
sprite_shift4_f2        equ	bitplanes+$85A0
sprite_shift5_f2        equ	bitplanes+$88A0
sprite_shift6_f2        equ	bitplanes+$8BA0
sprite_shift7_f2        equ	bitplanes+$8EA0
sprite_masked_shift0_f2 equ	bitplanes+$91A0
sprite_masked_shift1_f2 equ	bitplanes+$9560
sprite_masked_shift2_f2 equ	bitplanes+$9920
sprite_masked_shift3_f2 equ	bitplanes+$9CE0
sprite_masked_shift4_f2 equ	bitplanes+$A0A0
sprite_masked_shift5_f2 equ	bitplanes+$A460
sprite_masked_shift6_f2 equ	bitplanes+$A820
sprite_masked_shift7_f2 equ	bitplanes+$ABE0
sprite_f3               equ	bitplanes+$AFA0
sprite_masked_f3        equ	bitplanes+$B1A0
sprite_shift0_f3        equ	bitplanes+$B420
sprite_shift1_f3        equ	bitplanes+$B720
sprite_shift2_f3        equ	bitplanes+$BA20
sprite_shift3_f3        equ	bitplanes+$BD20
sprite_shift4_f3        equ	bitplanes+$C020
sprite_shift5_f3        equ	bitplanes+$C320
sprite_shift6_f3        equ	bitplanes+$C620
sprite_shift7_f3        equ	bitplanes+$C920
sprite_masked_shift0_f3 equ	bitplanes+$CC20
sprite_masked_shift1_f3 equ	bitplanes+$CFE0
sprite_masked_shift2_f3 equ	bitplanes+$D3A0
sprite_masked_shift3_f3 equ	bitplanes+$D760
sprite_masked_shift4_f3 equ	bitplanes+$DB20
sprite_masked_shift5_f3 equ	bitplanes+$DEE0
sprite_masked_shift6_f3 equ	bitplanes+$E2A0
sprite_masked_shift7_f3 equ	bitplanes+$E660
sprite_f4               equ	bitplanes+$EA20
sprite_masked_f4        equ	bitplanes+$EC20
sprite_shift0_f4        equ	bitplanes+$EEA0
sprite_shift1_f4        equ	bitplanes+$F1A0
sprite_shift2_f4        equ	bitplanes+$F4A0
sprite_shift3_f4        equ	bitplanes+$F7A0
sprite_shift4_f4        equ	bitplanes+$FAA0
sprite_shift5_f4        equ	bitplanes+$FDA0
sprite_shift6_f4        equ	bitplanes+$100A0
sprite_shift7_f4        equ	bitplanes+$103A0
sprite_masked_shift0_f4 equ	bitplanes+$106A0
sprite_masked_shift1_f4 equ	bitplanes+$10A60
sprite_masked_shift2_f4 equ	bitplanes+$10E20
sprite_masked_shift3_f4 equ	bitplanes+$111E0
sprite_masked_shift4_f4 equ	bitplanes+$115A0
sprite_masked_shift5_f4 equ	bitplanes+$11960
sprite_masked_shift6_f4 equ	bitplanes+$11D20
sprite_masked_shift7_f4 equ	bitplanes+$120E0
sprite_f5               equ	bitplanes+$124A0
sprite_masked_f5        equ	bitplanes+$126A0
sprite_shift0_f5        equ	bitplanes+$12920
sprite_shift1_f5        equ	bitplanes+$12C20
sprite_shift2_f5        equ	bitplanes+$12F20
sprite_shift3_f5        equ	bitplanes+$13220
sprite_shift4_f5        equ	bitplanes+$13520
sprite_shift5_f5        equ	bitplanes+$13820
sprite_shift6_f5        equ	bitplanes+$13B20
sprite_shift7_f5        equ	bitplanes+$13E20
sprite_masked_shift0_f5 equ	bitplanes+$14120
sprite_masked_shift1_f5 equ	bitplanes+$144E0
sprite_masked_shift2_f5 equ	bitplanes+$148A0
sprite_masked_shift3_f5 equ	bitplanes+$14C60
sprite_masked_shift4_f5 equ	bitplanes+$15020
sprite_masked_shift5_f5 equ	bitplanes+$153E0
sprite_masked_shift6_f5 equ	bitplanes+$157A0
sprite_masked_shift7_f5 equ	bitplanes+$15B60

saved_palette:				; where the old colours are kept
	dc.w	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
saved_ssp:
	dc.l	0
anim_frame:				; the animation state, in memory: the
	dc.w	0			;   blit's own registers are all scratch
anim_ticks:
	dc.w	0

msg_res:
	dc.b	'PiST scroll demo: this wants ST low resolution, 320x200 with 4 planes.',13,10
	dc.b	'Press a key to go back to TOS.',13,10,0
	even
