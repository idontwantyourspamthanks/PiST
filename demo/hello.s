* hello.s — PiST first-run demo
* =================================
*
* A tiny GEMDOS program, here so you can see the whole loop on a first run:
*
*   F7  build        (assembles with vasm, maps errors to lines)
*   F5  run / debug  (starts Hatari; click in the gutter to set a breakpoint)
*   F10 step,  F11 step over,  F9 continue
*
* Try a breakpoint on the `addq.w #1,d0` line and press F10 a few times:
* the register view shows D0 counting up.

_start:
	move.l	#msg,-(a7)		; Cconws(string): print to the screen
	move.w	#9,-(a7)
	trap	#1
	addq.l	#6,a7

	moveq	#0,d0			; a small loop to step through
	moveq	#10,d1			;   d0 counts 0..9 in d1's shadow
count:
	addq.w	#1,d0			; <-- breakpoint here is fun
	cmp.w	d1,d0
	blo.s	count

	move.l	#done,-(a7)		; report the result
	move.w	#9,-(a7)
	trap	#1
	addq.l	#6,a7

	move.w	#7,-(a7)		; Crawcin(): wait for a key so you can read it
	trap	#1
	addq.l	#2,a7

	clr.w	-(a7)			; Pterm0(): exit
	trap	#1

msg:
	dc.b	27,'E'				; clear screen
	dc.b	'Hello from PiST',13,10
	dc.b	'Counting to 10...',13,10,0
done:
	dc.b	'Done. Press a key to exit.',13,10,0
	even
