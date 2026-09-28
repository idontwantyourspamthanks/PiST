; mediademo guest: waits for one keypress, then turns the background red.
; Proof the media channel's input path works end to end: the 'a' you type
; reaches Cconin here, and Setcolor repaints — visible in the panel.

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

loop:	bra.s	loop
	end
