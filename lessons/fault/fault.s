* fault.s — a deliberately wrong program, for the debugging lesson
* ==================================================================
*
* The comment on the `moveq` below promises this adds 10 to D1. It adds 11.
* `dbf` decrements its counter first and loops while the counter is not -1,
* so counting 10 down to -1 runs the body 11 times. Nothing here is broken
* in a way the assembler can see, which is the whole point: finding it means
* reading the machine, not the source.
*
* Try it before reading the lesson: put a breakpoint on the `dbf` line, run,
* and watch D0 fall — then a breakpoint on the `clr.w` below it and look at
* D1.

kMax	equ	10

_start:
	moveq	#kMax,d0			; the count the comment promises
	moveq	#0,d1				; the running total
.loop:
	addq.l	#1,d1
	dbf	d0,.loop			; 11 times, not 10

	clr.w	-(a7)				; GEMDOS Pterm0: exit to TOS
	trap	#1
