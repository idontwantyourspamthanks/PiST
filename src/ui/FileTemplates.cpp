// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/FileTemplates.h"

#include <QObject>

namespace pist {

namespace {

/// GEMDOS hello: the Ctrl+N → Enter → F5 arc a first-time user takes. Shaped
/// on `demo/hello.s`, minus the teaching comments about the IDE: the listing
/// itself stays the thing you read, and the demo keeps the annotated version.
const char *const kGemdosHello = R"ASM(* hello.s — a PiST source, from the New File templates
*
* F7 assembles it, F5 builds and runs it in the emulator: TOS boots, this
* prints a line and waits for a key so you can read it.
*
* Breakpoints are file:line — click the gutter, or press F8 on a line. The
* interesting first move is one on the `move.w	#7,-(a7)` line: F5 stops there
* with the message already printed and the registers live.

_start:
	move.l	#message,-(a7)		; GEMDOS Cconws: print a string
	move.w	#9,-(a7)
	trap	#1
	addq.l	#6,a7

	move.w	#7,-(a7)		; GEMDOS Crawcin: wait for a key
	trap	#1
	addq.l	#2,a7

	clr.w	-(a7)			; GEMDOS Pterm0: exit to TOS
	trap	#1

message:
	dc.b	'Hello from PiST',13,10
	dc.b	'Press a key to exit.',13,10,0
	even
)ASM";

/// Keyboard/input: the input half of `demo/keyred.s`. TOS echoes the character
/// Cconin returns, so the loop is the whole program — and on the media channel
/// the emulator's display takes the keyboard while a session runs.
const char *const kKeyboardInput = R"ASM(* keyboard.s — a PiST source, from the New File templates
*
* Reads a key at a time with GEMDOS Cconin (TOS echoes it to the screen) and
* loops until ESC. F5 runs it; the emulator's display takes the keyboard while
* the session is up.
*
* The ST's hardware registers are a different story: they bus-error in user
* mode, so anything that touches them — the palette, the PSG, the screen base
* — goes through GEMDOS Super(0) first. `demo/keyred.s` has that shape.

_start:
	pea	message
	move.w	#9,-(a7)		; GEMDOS Cconws: print a string
	trap	#1
	addq.l	#6,a7

loop:
	move.w	#1,-(a7)		; GEMDOS Cconin: wait for a key, echoed
	trap	#1
	addq.l	#2,a7

	cmp.b	#$1b,d0			; $1b is ESC: leave
	beq.s	done
	bra.s	loop

done:
	clr.w	-(a7)			; GEMDOS Pterm0: exit to TOS
	trap	#1

message:
	dc.b	'Press keys; ESC quits.',13,10,0
	even
)ASM";

/// Image-include: the pipeline shape of `demo/spritedemo/sprites.s` — the
/// bitplanes PiST's sprite editor exports, pulled in with incbin and blitted.
/// The blit is the generated scroller's own idiom (mask ANDed into the screen,
/// planes ORed over it), trimmed to one frame and one sprite.
const char *const kImageInclude = R"ASM(; ---------------------------------------------------------------------------
; sprites.s — a PiST source, from the New File templates
;
; The shape PiST's sprite pipeline takes. Draw a sheet in the image editor,
; then File ▸ Export Bitplanes… writes <name>.dat beside this source: the
; palette first, then the blocks you ticked. The export dialog's file map
; lists every block's label and offset, and this listing pulls the .dat in
; with `incbin` and draws with it.
;
; The two `equ`s at the end are that map, trimmed to what this program uses.
; Re-export after editing the sheet: the file is rewritten, so the offsets
; move and the numbers here move with them.
;
; A 16-pixel group in the data is a mask word followed by four plane words.
; The blit ANDs the mask into the screen and ORs the planes over it, so a set
; mask bit keeps whatever was on the screen underneath.
;
; Supervisor mode is not optional: on the ST the GLUE bus-errors user-mode
; access to the system area and to the hardware registers, so the palette and
; the screen base can only be reached after GEMDOS Super(0).
; ---------------------------------------------------------------------------

kScreenRowBytes	equ	160		; one screen row: 320/16 groups of four plane words
kScreenBytes	equ	32000		; the whole screen, cleared at startup
kSpriteGroups	equ	8		; 16-pixel groups across the sprite
kSpriteHeight	equ	16		; painted rows
kSpriteY	equ	92		; the row it is drawn on: (200-16)/2

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

	lea	palette,a1		; ...and install the exported ones
	lea	$ffff8240,a0
	movem.l	(a1),d0-d7
	movem.l	d0-d7,(a0)

	move.w	#3,-(sp)		; XBIOS Logbase: the screen TOS draws to
	trap	#14
	addq.l	#2,sp
	movea.l	d0,a6

	move.l	a6,a1			; start from a clean screen
	move.w	#(kScreenBytes/4)-1,d0
.clear:
	clr.l	(a1)+
	dbf	d0,.clear

	bsr	blit			; draw the sprite once

	move.w	#7,-(sp)		; GEMDOS Crawcin: hold the screen so it
	trap	#1			;   can be looked at
	addq.l	#2,sp

	lea	saved_palette,a1	; put the colours back
	movem.l	(a1),d0-d7
	lea	$ffff8240,a0
	movem.l	d0-d7,(a0)

	move.l	saved_ssp,-(sp)		; GEMDOS Super(old_ssp): back to user mode
	move.w	#$20,-(sp)
	trap	#1
	addq.l	#6,sp

	clr.w	-(sp)			; GEMDOS Pterm0: exit to TOS
	trap	#1

; One data row per screen row, one 16-pixel group at a time. Registers:
; a0 = data, a1 = screen, a6 = screen base, d1/d3 scratch.
blit:
	lea	sprite_block,a0
	movea.l	a6,a1
	adda.l	#kSpriteY*kScreenRowBytes,a1
	move.w	#kSpriteHeight-1,d7
.row:
	move.w	#kSpriteGroups-1,d6
.group:
	move.w	(a0)+,d3		; the mask, kept for all four planes
	move.w	(a0)+,d1		; plane 0
	and.w	d3,(a1)
	or.w	d1,(a1)+
	move.w	(a0)+,d1		; plane 1
	and.w	d3,(a1)
	or.w	d1,(a1)+
	move.w	(a0)+,d1		; plane 2
	and.w	d3,(a1)
	or.w	d1,(a1)+
	move.w	(a0)+,d1		; plane 3
	and.w	d3,(a1)
	or.w	d1,(a1)+
	dbf	d6,.group
	adda.l	#(kScreenRowBytes-kSpriteGroups*8),a1
	dbf	d7,.row
	rts

saved_palette:
	dc.w	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
saved_ssp:
	dc.l	0

; The export's data, and the two addresses into it that this program uses —
; the offsets the export dialog's file map printed, measured from the start of
; the blob.
bitplanes:
	incbin	"sprites.dat"
palette		equ	bitplanes+$0000		; the ST palette the export wrote: 16 words (32 bytes)
sprite_block	equ	bitplanes+$0020		; one masked sprite: mask + four planes per group, and the
						; block the map puts first when only the palette precedes it
)ASM";

} // namespace

QList<SourceTemplate> allSourceTemplates()
{
    return { SourceTemplate::Blank,
             SourceTemplate::GemdosHello,
             SourceTemplate::KeyboardInput,
             SourceTemplate::ImageInclude };
}

QString templateLabel(SourceTemplate kind)
{
    switch (kind) {
    case SourceTemplate::Blank:
        return QObject::tr("Blank listing");
    case SourceTemplate::GemdosHello:
        return QObject::tr("GEMDOS hello");
    case SourceTemplate::KeyboardInput:
        return QObject::tr("Keyboard/input skeleton");
    case SourceTemplate::ImageInclude:
        return QObject::tr("Image-include listing");
    }
    return {};
}

QString templateDescription(SourceTemplate kind)
{
    switch (kind) {
    case SourceTemplate::Blank:
        return QObject::tr("An empty file: just the name and the directory.");
    case SourceTemplate::GemdosHello:
        return QObject::tr("Prints a line, waits for a key, exits — build it and run it "
                           "to see the whole loop work.");
    case SourceTemplate::KeyboardInput:
        return QObject::tr("Reads keys with GEMDOS Cconin until ESC.");
    case SourceTemplate::ImageInclude:
        return QObject::tr("Draws a sprite out of the bitplanes the image editor exports "
                           "to a .dat beside the source.");
    }
    return {};
}

QByteArray templateBytes(SourceTemplate kind)
{
    switch (kind) {
    case SourceTemplate::Blank:
        return {};
    case SourceTemplate::GemdosHello:
        return QByteArray(kGemdosHello);
    case SourceTemplate::KeyboardInput:
        return QByteArray(kKeyboardInput);
    case SourceTemplate::ImageInclude:
        return QByteArray(kImageInclude);
    }
    return {};
}

} // namespace pist
