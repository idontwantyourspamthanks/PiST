* hello.s — PiST's first-run demo, and the tour of what 0.10.0 added
* =================================================================
*
* A complete little GEMDOS program: it clears the screen, colours it, counts
* three rounds into a labelled word, prints one dot per round through GEMDOS,
* marks the end with a BIOS character, colours and uncolours the screen through
* XBIOS, and exits through Pterm0. Nothing here is scaffolding — it is the whole
* program a first run builds and runs — and every comment that names a feature
* sits directly above the line that shows it. CI assembles exactly this file
* with `vasmm68k_mot -quiet -Ftos`.
*
* The session, from the keyboard:
*
*   F7   build              assembles this file with vasm, errors on their lines
*   F5   run / debug        boots Hatari with the built program
*   F8   breakpoint         (or click the gutter; Shift+F9 clears them)
*   F10  step,  F11 step over,  F9 continue
*
* The three traps below are written as the ordinary idiom — push the arguments,
* push the function number, `trap`, drop what was pushed — because the reference
* dock and the hover card find a call by reading that sequence back. Put the
* caret or the pointer on a `trap #N` line, or on a push feeding it, and it
* names the call, its prototype, and the arguments this very line pushes.

	text

* `rounds` is an equate, not a label: it emits no bytes, so it has no address.
* Completion (Ctrl+Space, or Tab on a word) offers it here and below, and Go to
* Symbol… (Ctrl+Shift+O) lists it with its file:line. A hook script's sym()
* refuses an equate for exactly that reason, which is why hello.hooks.lua asks
* for the labels `counter` and `hook_calls` instead.
rounds	equ	3			; rounds the loop below counts to


_start:

* --- GEMDOS (trap #1): the banner, and the arc every PiST program shares ----
* Hover the `trap #1` line: the card names GEMDOS Cconws, its prototype, and
* the argument pushed above it. This is the same shape the New File template
* writes for a GEMDOS program, and the reference dock shows the same entry with
* the Example: and Typical use: lines authored for it.
	move.l	#banner,-(sp)		; Argument: the NUL-terminated string
	move.w	#9,-(sp)		; GEMDOS function 9: Cconws
	trap	#1
	addq.l	#6,sp

* --- XBIOS (trap #14): the screen changes colour under the banner -----------
* Hover the `trap #14` line below and the card names XBIOS Setcolor, with its
* prototype and the two arguments pushed above it. Colour 0 is the background
* and $0700 is full red in the ST's word — red is bits 8-10 (cf.
* src/image/Palette.cpp); keyred.s proves the same thing with the same value.
* The GEMDOS block further down puts it back, so the machine is left as found.
	move.w	#$0700,-(sp)		; color: full red
	move.w	#0,-(sp)		; colornum: register 0, the background
	move.w	#7,-(sp)		; XBIOS function 7: Setcolor
	trap	#14
	addq.l	#6,sp

* --- the loop, and the word to watch by name --------------------------------
* Set a breakpoint on the `addq.w` line (F8), press F5, then F10: d0 climbs in
* the register view, and the console gets one dot per round.
*
* `counter` is the word the watchpoint symbols of this release exist for: Run ▸
* Add watchpoint…, type counter.w, and the panel arms it at the stop that
* reports the program's bases. A watchpoint asked for by name is re-resolved on
* every run, so it survives the address GEMDOS picks afresh each time; one typed
* in as an address does not. It fires on each round below, and once more when
* hello.hooks.lua pokes it.
*
* Find in Files needs nothing from this file: Ctrl+Shift+F (Search ▸ Find in
* Files…), search `count`, and the dock finds this label with its line.
	moveq	#0,d0			; d0 counts the rounds (0..rounds)
count:
	addq.w	#1,d0			; <-- a breakpoint here is the tour's centre
	move.w	d0,counter		; store the count in the watched word
	; GEMDOS Cconout (2): one '.' straight to con: per round
	move.w	#46,-(sp)		; Argument: '.'
	move.w	#2,-(sp)		; GEMDOS function 2: Cconout
	trap	#1
	addq.l	#4,sp
	cmp.w	#rounds,d0
	blo.s	count

* --- BIOS (trap #13): the '!' the dots were counting up to ------------------
* A trap #13 skips GEMDOS and talks to the BIOS device layer instead: hover it
* and the card says Bconout(int16_t dev, int16_t c), device 2 being the screen.
	move.w	#33,-(sp)		; c: '!'
	move.w	#2,-(sp)		; dev: 2, the screen
	move.w	#3,-(sp)		; BIOS function 3: Bconout
	trap	#13
	addq.l	#6,sp

* --- GEMDOS again: report, put the screen back, wait, and leave -------------
	move.l	#done,-(sp)
	move.w	#9,-(sp)		; GEMDOS function 9: Cconws
	trap	#1
	addq.l	#6,sp

	move.w	#$0777,-(sp)		; color: white, the way the machine started
	move.w	#0,-(sp)		; colornum: the background again
	move.w	#7,-(sp)		; XBIOS function 7: Setcolor
	trap	#14
	addq.l	#6,sp

	; GEMDOS Crawcin (7): wait for a key, so the screen can be read
	move.w	#7,-(sp)
	trap	#1
	addq.l	#2,sp

	; GEMDOS Pterm0 (0): hand the machine back to TOS. Pterm0 never returns,
	; so nothing is dropped from the stack after it.
	clr.w	-(sp)
	trap	#1

* --- constants: read-only strings, so they live in `text` with the code -----
banner:
	dc.b	27,'E'			; VT52 escape: clear the screen, home the cursor
	dc.b	'Hello from PiST 0.10.0',13,10
	dc.b	'Counting rounds: ',0
done:
	dc.b	' done.',13,10
	dc.b	'Press a key to exit.',13,10,0
	even

* --- writable state: the two symbols the IDE wants by name ------------------
* `text` above is this program's code and its constants; `data` is the state a
* run changes, which is exactly what has to be separate.
*
* `hook_calls` is the stop-hook declaration: Run ▸ Stop hooks… is already
* pointing at hello.hooks.lua for this project (hooks.scriptPath in
* hello.pistproject), and hello.hooks.lua logs every stop, counts the stops in
* this byte and pokes `counter` on its second stop. Read that file next — its
* comments are the Lua API reference.
*
* The floppy-pane edits this release added are not a source feature at all:
* right-click inside the Disk A or Disk B pane to Delete…, Rename… or New
* Folder… in a mounted image. They are named here only so the tour is complete.
	data

counter:	dc.w	0	; rounds counted so far — watch this one by name
hook_calls:	dc.b	0	; stops hello.hooks.lua has run through; one
				;   byte, because writemem() writes one at a time
	even
