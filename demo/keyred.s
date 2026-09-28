; mediademo guest: waits for a keypress, turns the background red and plays a
; ~440 Hz beep — one pass per key, so every keypress proves the media
; channel's input path (echo + red) and its audio path (beep) at once.

	text
start:
	; GEMDOS Cconin (0x01): wait for one key (echoed by TOS)
	move.w	#1,-(sp)
	trap	#1
	addq.l	#2,sp

	; XBIOS Setcolor (7): palette register 0 (background) <- $0700 (full red;
	; the STfm word keeps red in bits 8-10, cf. src/image/Palette.cpp)
	move.w	#$0700,-(sp)
	move.w	#0,-(sp)
	move.w	#7,-(sp)
	trap	#14
	addq.l	#6,sp

	; The PSG lives at $FFFF8800, which a user-mode program may not touch;
	; run the sound part under Supexec (XBIOS 38).
	pea	beep
	move.w	#38,-(sp)
	trap	#14
	addq.l	#6,sp

	bra.s	start

; --- supervisor: one ~440 Hz beep, ~0.5 s -------------------------------
; Channel A period $011C (F = 2 MHz / (16 * TP)); tone A only; volume 15.
beep:
	move.b	#0,$FFFF8800
	move.b	#$1C,$FFFF8802
	move.b	#1,$FFFF8800
	move.b	#$01,$FFFF8802
	move.b	#7,$FFFF8800
	move.b	#$3E,$FFFF8802
	move.b	#8,$FFFF8800
	move.b	#15,$FFFF8802

	move.w	#6,d0		; ~0.5 s at 8 MHz (d0=200 holds ~16 s and
				; would eat every later keypress until it returns)
.outer:	move.w	#$FFFF,d1
.inner:	dbra	d1,.inner
	dbra	d0,.outer

	move.b	#8,$FFFF8800
	move.b	#0,$FFFF8802
	rts
	end
