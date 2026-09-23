// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/OsCallRef.h"

#include <QHash>

namespace pist {

namespace {

/// Append one entry and return it, so a call whose canonical binding has an
/// irregular shape can set the layout member that describes it (see OsCallInfo).
/// The reference stays valid until the next append, and every caller uses it
/// immediately.
OsCallInfo &add(QList<OsCallInfo> &table, int trap, int opcode, const char *name,
                const char *prototype, const char *summary, const char *returns,
                const char *availability, int stackBytes)
{
    table.append(OsCallInfo{trap, opcode, QString::fromLatin1(name), QString::fromLatin1(prototype),
                            QString::fromLatin1(summary), QString::fromLatin1(returns),
                            QString::fromLatin1(availability), stackBytes});
    return table.last();
}

// Phrases repeated across whole families, so a family reads consistently.
const char *const noReturn = "";

/// The reference, grouped the way a manual groups it: GEMDOS by name prefix,
/// then XBIOS and BIOS by subsystem. Building it once at first use keeps the
/// list out of the static-init order, which would matter for the QString
/// members.
///
/// Function numbers, names, signatures and stack layouts are verified against
/// tos.hyp (freemint.github.io/tos.hyp, GPLv2) and Atari's 1986 GEMDOS
/// Reference Manual; the summaries are our own words. Scope is the TOS 1.x/2.x
/// set; later machines' calls (TOS 4, TT, Falcon) are deliberately absent.
QList<OsCallInfo> buildTable()
{
    QList<OsCallInfo> t;
    t.reserve(111);

    // --- GEMDOS: character input/output -------------------------------------
    add(t, 1, 1, "Cconin", "int32_t Cconin(void)",
        "Read a character from con:, echoing it; waits for a key.",
        "ASCII in bits 0-7 (0 for non-ASCII keys), scancode in 16-23", "", 2);
    add(t, 1, 2, "Cconout", "int32_t Cconout(int16_t c)",
        "Write one character to con: (VT52 sequences are interpreted).", noReturn, "", 4);
    add(t, 1, 3, "Cauxin", "int32_t Cauxin(void)",
        "Read a character from aux: (the serial port); waits.", "character in the low byte", "", 2);
    add(t, 1, 4, "Cauxout", "int32_t Cauxout(int16_t c)",
        "Write one character to aux: (the serial port).", "negative on error", "", 4);
    add(t, 1, 5, "Cprnout", "int32_t Cprnout(int16_t c)",
        "Write one character to prn: (the printer port).", "non-zero if written", "", 4);
    add(t, 1, 6, "Crawio", "int32_t Crawio(int16_t w)",
        "Raw console I/O: write character w, or read without waiting when w = $FF.",
        "with w = $FF: character and scancode, 0 if none waiting", "", 4);
    add(t, 1, 7, "Crawcin", "int32_t Crawcin(void)",
        "Read a character from con: without echo or control-key interpretation; waits.",
        "ASCII in bits 0-7, scancode in 16-23", "", 2);
    add(t, 1, 8, "Cnecin", "int32_t Cnecin(void)",
        "Read a character from con: without echo; control keys still apply; waits.",
        "ASCII in bits 0-7, scancode in 16-23", "", 2);
    add(t, 1, 9, "Cconws", "int32_t Cconws(const char *buf)",
        "Write a NUL-terminated string to con:.", "0 ok, -1 error (TOS 1.04)", "", 6);
    add(t, 1, 10, "Cconrs", "int32_t Cconrs(LINE *buf)",
        "Read an edited line from con:; buf[0] is the maximum length, buf[1] gets the count.",
        "0 ok, negative on error", "", 6);
    add(t, 1, 11, "Cconis", "int32_t Cconis(void)",
        "Test whether a character is waiting on con: input.", "-1 if one is waiting, 0 if not", "", 2);
    add(t, 1, 16, "Cconos", "int16_t Cconos(void)",
        "Test whether con: output can accept a character.", "1 if it can, 0 if not", "", 2);
    add(t, 1, 17, "Cprnos", "int16_t Cprnos(void)",
        "Test whether the printer is ready.", "-1 ready, 0 not ready", "", 2);
    add(t, 1, 18, "Cauxis", "int16_t Cauxis(void)",
        "Test whether a character is waiting on aux: input.", "-1 if one is buffered, 0 if none", "", 2);
    add(t, 1, 19, "Cauxos", "int16_t Cauxos(void)",
        "Test whether aux: output can accept a character.", "-1 if it can, 0 if not", "", 2);

    // --- GEMDOS: directories -------------------------------------------------
    add(t, 1, 14, "Dsetdrv", "int32_t Dsetdrv(int16_t drv)",
        "Set the current drive (0 = A:).", "bit-map of mounted drives", "", 4);
    add(t, 1, 25, "Dgetdrv", "int16_t Dgetdrv(void)",
        "Get the current drive (0 = A:).", "drive number", "", 2);
    add(t, 1, 54, "Dfree", "int16_t Dfree(DISKINFO *buf, int16_t driveno)",
        "Fill buf with a drive's free/total clusters, sector and cluster size (0 = current drive).",
        "0 ok, negative on error", "", 8);
    add(t, 1, 57, "Dcreate", "int32_t Dcreate(const char *path)",
        "Create a directory.", "0 ok, negative error code", "", 6);
    add(t, 1, 58, "Ddelete", "int32_t Ddelete(const char *path)",
        "Delete an empty directory.", "0 ok, negative error code", "", 6);
    add(t, 1, 59, "Dsetpath", "int16_t Dsetpath(const char *path)",
        "Set the current directory.", "0 ok, EPTHNF if not found", "", 6);
    add(t, 1, 71, "Dgetpath", "int16_t Dgetpath(char *path, int16_t driveno)",
        "Write a drive's current directory into path (0 = current drive).", "0 ok, EDRIVE", "", 8);

    // --- GEMDOS: memory -------------------------------------------------------
    add(t, 1, 20, "Maddalt", "int32_t Maddalt(void *start, int32_t size)",
        "Register alternative (TT/Fast) RAM with the memory allocator.",
        "0 ok, negative error code", "GEMDOS 0.19+ (TOS 2.01+)", 10);
    add(t, 1, 68, "Mxalloc", "void *Mxalloc(int32_t amount, int16_t mode)",
        "Allocate memory from a chosen region; amount = -1 queries the largest block for mode.",
        "block address, 0 on failure", "GEMDOS 0.19+ (TOS 2.01+)", 8);
    add(t, 1, 72, "Malloc", "void *Malloc(int32_t number)",
        "Allocate a block of memory; number = -1 queries the largest free block.",
        "block address, NULL on failure", "", 6);
    add(t, 1, 73, "Mfree", "int32_t Mfree(void *block)",
        "Release a block allocated with Malloc.", "0 ok, EIMBA", "", 6);
    add(t, 1, 74, "Mshrink", "int32_t Mshrink(void *block, int32_t newsiz)",
        "Shrink an allocated block to newsiz bytes; the binding pushes a reserved zero word after the size.",
        "0 ok, EIMBA or EGSBF", "", 12).reservedWords = 1;

    // --- GEMDOS: processes ----------------------------------------------------
    add(t, 1, 0, "Pterm0", "void Pterm0(void)",
        "Terminate the process with exit code 0.", noReturn, "", 2);
    add(t, 1, 32, "Super", "int32_t Super(void *stack)",
        "Switch between user and supervisor mode; stack = 1 only queries, 0 switches sharing the stack.",
        "mode when querying, else the old supervisor stack", "", 6);
    add(t, 1, 49, "Ptermres", "void Ptermres(int32_t keepcnt, int16_t retcode)",
        "Terminate but stay resident, keeping keepcnt bytes starting at the basepage.", noReturn, "", 8);
    add(t, 1, 75, "Pexec", "int32_t Pexec(uint16_t mode, ...)",
        "Load and/or run a program: mode 0 load+go, 3 load only, 4 go, 5 create basepage, 7 with prgflags.",
        "mode 0/4: child exit code; mode 3/5/7: basepage pointer", "", 16).fixedArgBlock = true;
    add(t, 1, 76, "Pterm", "void Pterm(uint16_t retcode)",
        "Terminate the process with an exit code.", noReturn, "", 4);

    // --- GEMDOS: date and time ------------------------------------------------
    add(t, 1, 42, "Tgetdate", "uint16_t Tgetdate(void)",
        "Read the current date.", "packed: bits 0-4 day, 5-8 month, 9-15 year since 1980", "", 2);
    add(t, 1, 43, "Tsetdate", "int32_t Tsetdate(uint16_t date)",
        "Set the current date, packed as in Tgetdate.", "0 ok, -1 invalid", "", 4);
    add(t, 1, 44, "Tgettime", "uint16_t Tgettime(void)",
        "Read the current time.", "packed: bits 0-4 seconds/2, 5-10 minutes, 11-15 hours", "", 2);
    add(t, 1, 45, "Tsettime", "int32_t Tsettime(uint16_t time)",
        "Set the current time, packed as in Tgettime.", "0 ok, -1 invalid", "", 4);

    // --- GEMDOS: files ---------------------------------------------------------
    add(t, 1, 26, "Fsetdta", "void Fsetdta(DTA *buf)",
        "Set the disk transfer address that Fsfirst/Fsnext fill in.", noReturn, "", 6);
    add(t, 1, 47, "Fgetdta", "DTA *Fgetdta(void)",
        "Get the current disk transfer address.", "DTA pointer", "", 2);
    add(t, 1, 60, "Fcreate", "int16_t Fcreate(const char *fname, int16_t attr)",
        "Create or truncate a file with the given attributes.", "file handle, negative error code", "", 8);
    add(t, 1, 61, "Fopen", "int32_t Fopen(const char *fname, int16_t mode)",
        "Open a file (mode 0 read, 1 write, 2 read/write).", "file handle, negative error code", "", 8);
    add(t, 1, 62, "Fclose", "int16_t Fclose(int16_t handle)",
        "Close an open file handle.", "0 ok, EBADF", "", 4);
    add(t, 1, 63, "Fread", "int32_t Fread(int16_t handle, int32_t count, void *buf)",
        "Read up to count bytes from a file into buf.", "bytes actually read, negative error code", "", 12);
    add(t, 1, 64, "Fwrite", "int32_t Fwrite(int16_t handle, int32_t count, void *buf)",
        "Write count bytes from buf to a file.", "bytes actually written, negative error code", "", 12);
    add(t, 1, 65, "Fdelete", "int16_t Fdelete(const char *fname)",
        "Delete a file.", "0 ok, EFILNF or EACCDN", "", 6);
    add(t, 1, 66, "Fseek", "int32_t Fseek(int32_t offset, int16_t handle, int16_t seekmode)",
        "Move a file's position (seekmode 0 absolute, 1 relative, 2 from end).",
        "new absolute position, negative error code", "", 10);
    add(t, 1, 67, "Fattrib", "int16_t Fattrib(const char *filename, int16_t wflag, int16_t attrib)",
        "Read a file's attributes, or set them when wflag is 1.",
        "current attributes, negative error code", "", 10);
    add(t, 1, 69, "Fdup", "int16_t Fdup(int16_t handle)",
        "Duplicate a file handle.", "the new handle, negative error code", "", 4);
    add(t, 1, 70, "Fforce", "int16_t Fforce(int16_t stdh, int16_t nonstdh)",
        "Redirect a standard handle (0-2) to another handle.", "0 ok, EBADF", "", 6);
    add(t, 1, 78, "Fsfirst", "int32_t Fsfirst(const char *filename, int16_t attr)",
        "Find the first file matching a wildcard path; fills the DTA.", "0 ok, EFILNF or ENMFIL", "", 8);
    add(t, 1, 79, "Fsnext", "int16_t Fsnext(void)",
        "Find the next match after Fsfirst; fills the DTA.", "0 if found, negative error code", "", 2);
    add(t, 1, 86, "Frename", "int32_t Frename(const char *oldname, const char *newname)",
        "Rename a file; the binding pushes a reserved zero word after oldname.",
        "0 ok, negative error code", "", 12).reservedWords = 1;
    add(t, 1, 87, "Fdatime", "void Fdatime(DOSTIME *timeptr, int16_t handle, int16_t wflag)",
        "Read a file's timestamp, or set it when wflag is 1.", noReturn, "", 10);

    // --- GEMDOS: system --------------------------------------------------------
    add(t, 1, 48, "Sversion", "uint16_t Sversion(void)",
        "Read the GEMDOS version number.", "BCD version: low byte major, high byte minor", "", 2);

    // --- XBIOS: screen ---------------------------------------------------------
    add(t, 14, 0, "Initmouse", "void Initmouse(int16_t type, MOUSE *par, void (*mousevec)())",
        "Initialise the mouse handler (type 0 off, 1 relative, 2 absolute, 4 key-code).", noReturn, "", 12);
    add(t, 14, 2, "Physbase", "void *Physbase(void)",
        "Get the address of the physical screen memory.", "screen address", "", 2);
    add(t, 14, 3, "Logbase", "void *Logbase(void)",
        "Get the address of the logical screen memory (what the VDI draws to).", "screen address", "", 2);
    add(t, 14, 4, "Getrez", "int16_t Getrez(void)",
        "Get the current screen resolution.", "0 = 320x200, 1 = 640x200, 2 = 640x400", "", 2);
    add(t, 14, 5, "Setscreen", "void Setscreen(void *laddr, void *paddr, int16_t rez)",
        "Set logical/physical screen addresses and resolution; -1 leaves an item unchanged.",
        noReturn, "", 12);
    add(t, 14, 6, "Setpalette", "void Setpalette(void *pallptr)",
        "Queue 16 colours for the next vertical blank; the pointer must stay valid until then.",
        noReturn, "", 6);
    add(t, 14, 7, "Setcolor", "int16_t Setcolor(int16_t colornum, int16_t color)",
        "Set one palette register; color = -1 only reads it.", "old register value", "", 6);
    add(t, 14, 21, "Cursconf", "int16_t Cursconf(int16_t func, int16_t rate)",
        "Configure the text cursor (0 off, 1 on, 2/3 blink, 4-7 rate and delay).",
        "rate or delay when func is 5 or 7", "", 6);
    add(t, 14, 37, "Vsync", "void Vsync(void)",
        "Wait for the next vertical blank.", noReturn, "", 2);

    // --- XBIOS: special --------------------------------------------------------
    add(t, 14, 1, "Ssbrk", "void *Ssbrk(int16_t count)",
        "Reserve memory before GEMDOS starts; a dummy routine in ROM TOS.", "block address", "", 4);
    // Dbmsg's first argument is its reserved word, and its value must be 5: the
    // binding pushes that literal rather than a placeholder for the caller.
    OsCallInfo &dbmsg = add(t, 14, 11, "Dbmsg",
                            "void Dbmsg(int16_t rsrvd, int16_t msg_num, int32_t msg_arg)",
                            "Send a message to a resident debugger; rsrvd must be 5, msg_num "
                            "selects the operation.",
                            noReturn, "with a resident debugger (the Atari Debugger)", 10);
    dbmsg.reservedArg = 0;
    dbmsg.reservedArgValue = 5;
    add(t, 14, 17, "Random", "int32_t Random(void)",
        "Return a 24-bit pseudo-random number (software generator).", "random value in bits 0-23", "", 2);
    add(t, 14, 38, "Supexec", "int32_t Supexec(int32_t (*func)())",
        "Run func in supervisor mode and return its result; trashes d0-d2.", "func's return value", "", 6);
    add(t, 14, 39, "Puntaes", "void Puntaes(void)",
        "Unhook a disk-loaded AES and reboot.", noReturn, "", 2);
    add(t, 14, 64, "Blitmode", "int16_t Blitmode(int16_t mode)",
        "Query or configure the blitter; mode = -1 reads the state.",
        "bit 0 = on, bit 1 = present", "TOS 1.02+", 4);

    // --- XBIOS: drives ----------------------------------------------------------
    add(t, 14, 8, "Floprd", "int16_t Floprd(void *buf, int32_t filler, int16_t devno, int16_t sectno, int16_t trackno, int16_t sideno, int16_t count)",
        "Read floppy sectors into buf; filler is unused, push 0.", "0 ok, non-zero error", "", 20);
    add(t, 14, 9, "Flopwr", "int16_t Flopwr(void *buf, int32_t filler, int16_t devno, int16_t sectno, int16_t trackno, int16_t sideno, int16_t count)",
        "Write floppy sectors from buf; same layout as Floprd.", "0 ok, non-zero error", "", 20);
    add(t, 14, 10, "Flopfmt", "int16_t Flopfmt(void *buf, int32_t filler, int16_t devno, int16_t spt, int16_t trackno, int16_t sideno, int16_t interlv, int32_t magic, int16_t virgin)",
        "Format a floppy track; magic must be $87654321 or nothing happens.",
        "0 ok; on failure buf gets the faulty sector list", "", 26);
    add(t, 14, 18, "Protobt", "void Protobt(void *buf, int32_t serialno, int16_t disktype, int16_t execflag)",
        "Prototype a boot sector in a 512-byte buffer; -1 leaves type/flag unchanged.", noReturn, "", 14);
    add(t, 14, 19, "Flopver", "int16_t Flopver(void *buf, int32_t filler, int16_t devno, int16_t sectno, int16_t trackno, int16_t sideno, int16_t count)",
        "Verify floppy sectors, collecting the defective ones into buf.",
        "0 if the bad-sector list is valid", "", 20);
    add(t, 14, 41, "Floprate", "int16_t Floprate(int16_t devno, int16_t newrate)",
        "Set a drive's seek rate.", "previous seek rate", "TOS 1.04+", 6);
    add(t, 14, 42, "DMAread", "int16_t DMAread(int32_t sector, int16_t count, void *buffer, int16_t devno)",
        "Read hard-disk sectors (devno 0-7 ACSI, 8-15 SCSI, 16 IDE).", "0 ok, negative error code", "TOS 2.00+", 14);
    add(t, 14, 43, "DMAwrite", "int16_t DMAwrite(int32_t sector, int16_t count, void *buffer, int16_t devno)",
        "Write hard-disk sectors; same layout as DMAread.", "0 ok, negative error code", "TOS 2.00+", 14);

    // --- XBIOS: interfaces -------------------------------------------------------
    add(t, 14, 12, "Midiws", "void Midiws(int16_t cnt, void *ptr)",
        "Write cnt+1 bytes to the MIDI port.", noReturn, "", 8);
    add(t, 14, 13, "Mfpint", "void Mfpint(int16_t number, int16_t (*vector)())",
        "Install a handler for one of the 16 MFP interrupt sources.", noReturn, "", 8);
    add(t, 14, 14, "Iorec", "IOREC *Iorec(int16_t dev)",
        "Get a device's IOREC buffer structure (0 RS-232, 1 keyboard, 2 MIDI).", "IOREC pointer", "", 4);
    add(t, 14, 15, "Rsconf", "int32_t Rsconf(int16_t baud, int16_t ctr, int16_t ucr, int16_t rsr, int16_t tsr, int16_t scr)",
        "Configure the serial port; -1 leaves a parameter unchanged, baud -2 reads the rate.",
        "old register values packed in d0", "", 14);
    add(t, 14, 25, "Ikbdws", "void Ikbdws(int16_t count, const int8_t *ptr)",
        "Send count+1 command bytes to the keyboard processor.", noReturn, "", 8);
    add(t, 14, 28, "Giaccess", "int8_t Giaccess(int16_t data, int16_t regno)",
        "Read a sound-chip register, or write it when bit 7 of regno is set.", "register contents", "", 6);
    add(t, 14, 29, "Offgibit", "void Offgibit(int16_t bitno)",
        "Clear bits of PSG port A.", noReturn, "", 4);
    add(t, 14, 30, "Ongibit", "void Ongibit(int16_t bitno)",
        "Set bits of PSG port A.", noReturn, "", 4);
    add(t, 14, 32, "Dosound", "void *Dosound(const int8_t *buf)",
        "Play a PSG command stream; buf must stay valid while it plays.",
        "previously playing buffer, if any", "", 6);
    add(t, 14, 34, "Kbdvbase", "KBDVBASE *Kbdvbase(void)",
        "Get the IKBD/MIDI vector table.", "KBDVBASE pointer", "", 2);
    add(t, 14, 44, "Bconmap", "int32_t Bconmap(int16_t devno)",
        "Map the serial console to another device; -1 queries, presence test is Bconmap(0) = 0.",
        "previous mapping, or BCONMAP pointer for -2", "TOS 2.00+", 4);

    // --- XBIOS: keyboard ---------------------------------------------------------
    add(t, 14, 16, "Keytbl", "KEYTAB *Keytbl(void *unshift, void *shift, void *capslock)",
        "Install keyboard translation tables; -1 leaves a table unchanged.", "previous KEYTAB", "", 14);
    add(t, 14, 24, "Bioskeys", "void Bioskeys(void)",
        "Restore the standard keyboard tables, undoing Keytbl.", noReturn, "", 2);
    add(t, 14, 35, "Kbrate", "int16_t Kbrate(int16_t initial, int16_t repeat)",
        "Set key repeat delay and rate in 20 ms steps; -1 leaves one unchanged.",
        "old rate in bits 0-7, delay in 8-15", "", 6);

    // --- XBIOS: date and time ----------------------------------------------------
    add(t, 14, 22, "Settime", "void Settime(uint32_t time)",
        "Set the date and time from one packed longword (seconds/2, then minutes, hours, day, month, year).",
        noReturn, "", 6);
    add(t, 14, 23, "Gettime", "uint32_t Gettime(void)",
        "Read the date and time as one packed longword.", "packed date/time as in Settime", "", 2);
    add(t, 14, 31, "Xbtimer", "void Xbtimer(int16_t timer, int16_t control, int16_t data, void (*vector)())",
        "Install an MFP timer handler (0-3 = timers A-D; 2 is the 200 Hz system timer).", noReturn, "", 12);
    add(t, 14, 47, "Waketime", "int32_t Waketime(int32_t time)",
        "Program the ST-Book's wake-up alarm: 0 clears, -1 reads, 1 enables, else sets a packed time.",
        "status code, or the stored alarm for -1", "TOS 2.06, ST-Book", 6);

    // --- XBIOS: printer -----------------------------------------------------------
    add(t, 14, 20, "Scrdmp", "void Scrdmp(void)",
        "Dump the screen to the printer; cancelled with Alternate+Help.", noReturn, "", 2);
    add(t, 14, 33, "Setprt", "int16_t Setprt(int16_t config)",
        "Configure the printer; config = -1 only reads it.", "old configuration word", "", 4);
    add(t, 14, 36, "Prtblk", "int16_t Prtblk(PBDEF *par)",
        "Print a block described by a PBDEF structure.", "0 ok, non-zero on failure", "", 6);

    // --- XBIOS: interrupts ----------------------------------------------------------
    add(t, 14, 26, "Jdisint", "void Jdisint(int16_t number)",
        "Disable MFP interrupt source number (0-15).", noReturn, "", 4);
    add(t, 14, 27, "Jenabint", "void Jenabint(int16_t number)",
        "Enable MFP interrupt source number (0-15).", noReturn, "", 4);

    // --- BIOS: character devices -----------------------------------------------------
    add(t, 13, 1, "Bconstat", "int16_t Bconstat(int16_t dev)",
        "Test whether input is pending on a device (0 prn, 1 aux, 2 con, 3 MIDI, 4 keyboard, 5 screen).",
        "-1 pending, 0 not", "", 4);
    add(t, 13, 2, "Bconin", "int32_t Bconin(int16_t dev)",
        "Read a character from a device; waits.", "character, plus scancode on con:", "", 4);
    add(t, 13, 3, "Bconout", "void Bconout(int16_t dev, int16_t c)",
        "Write one character to a device; returns once it is actually out.", noReturn, "", 6);
    add(t, 13, 8, "Bcostat", "int32_t Bcostat(int16_t dev)",
        "Test whether a device can accept output; dev 3 and 4 are swapped versus the other calls.",
        "-1 ready, 0 full", "", 4);

    // --- BIOS: disk --------------------------------------------------------------------
    add(t, 13, 4, "Rwabs", "int32_t Rwabs(int16_t rwflag, void *buff, int16_t cnt, int16_t recnr, int16_t dev, int32_t lrecno)",
        "Read or write disk sectors; rwflag bit 0 = write, bit 1 = ignore media change.",
        "0 ok, negative error code", "", 18);
    add(t, 13, 7, "Getbpb", "BPB *Getbpb(int16_t dev)",
        "Get a drive's BIOS parameter block; resets the media-change status.", "BPB pointer", "", 4);
    add(t, 13, 9, "Mediach", "int32_t Mediach(int16_t dev)",
        "Ask whether a drive's medium changed since the last access.",
        "0 no, 1 maybe, 2 definitely", "", 4);
    add(t, 13, 10, "Drvmap", "int32_t Drvmap(void)",
        "Read the bit-vector of mounted drives (the _drvbits system variable).",
        "drive bit-vector, bit 0 = A:", "", 2);

    // --- BIOS: system ---------------------------------------------------------------------
    add(t, 13, 0, "Getmpb", "void Getmpb(MPB *ptr)",
        "Fill in the memory parameter block; boot-time only, never call it afterwards.", noReturn, "", 6);
    add(t, 13, 5, "Setexc", "int32_t Setexc(int16_t number, void (*vec)())",
        "Set an exception vector; vec = -1 reads it without changing anything.",
        "previous vector contents", "", 8);
    add(t, 13, 6, "Tickcal", "int32_t Tickcal(void)",
        "Read the milliseconds between two system timer calls.", "milliseconds per tick", "", 2);
    add(t, 13, 11, "Kbshift", "int32_t Kbshift(int16_t mode)",
        "Read the shift-key state, or set it when mode is not negative.",
        "shift bits: 0-1 shift, 2 Ctrl, 3 Alt, 4 CapsLock", "", 4);

    return t;
}

} // namespace

const QList<OsCallInfo> &osCallTable()
{
    static const QList<OsCallInfo> table = buildTable();
    return table;
}

QString osCallLayerName(int trap)
{
    switch (trap) {
    case 1: return QStringLiteral("GEMDOS");
    case 13: return QStringLiteral("BIOS");
    case 14: return QStringLiteral("XBIOS");
    default: return QString();
    }
}

QString osCallLayerKey(int trap)
{
    switch (trap) {
    case 1: return QStringLiteral("gemdos");
    case 13: return QStringLiteral("bios");
    case 14: return QStringLiteral("xbios");
    default: return QString();
    }
}

namespace {

/// The two look-ups the scanner makes, hashed: a scan of all 111 entries with a
/// case-insensitive compare each is what these two were doing on the editor's
/// per-cursor path. Built from the table itself, so a row added above is
/// reachable at once and the index cannot drift from the listing — the shape
/// InstrRef uses for its mnemonics, for the same reason.
///
/// The trap number keys the outer hash in both indexes, which is what keeps the
/// layers apart: a look-up for a trap the table does not cover finds no inner
/// hash and answers nullptr, exactly as the linear scan did. Name keys are
/// lower-cased, which is what an ASCII identifier's case-insensitive compare
/// amounts to; both the table's names and the source words it resolves are ASCII.
struct OsCallIndex
{
    QHash<int, QHash<int, const OsCallInfo *>> byOpcode;
    QHash<int, QHash<QString, const OsCallInfo *>> byName;
};

const OsCallIndex &osCallIndex()
{
    static const OsCallIndex index = [] {
        OsCallIndex i;
        for (const OsCallInfo &info : osCallTable()) {
            i.byOpcode[info.trap].insert(info.opcode, &info);
            i.byName[info.trap].insert(info.name.toLower(), &info);
        }
        return i;
    }();
    return index;
}

} // namespace

const OsCallInfo *osCallRef(int trap, int opcode)
{
    return osCallIndex().byOpcode.value(trap).value(opcode, nullptr);
}

const OsCallInfo *osCallRefByName(int trap, const QString &name)
{
    return osCallIndex().byName.value(trap).value(name.toLower(), nullptr);
}

} // namespace pist
