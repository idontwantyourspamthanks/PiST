# macOS emulator: in-process libretro core

The decision, and the build that follows from it. Linux and Windows keep launching
Hatari as a separate process. macOS cannot put that process's window into the
Emulator panel (`WId` is a process-local `NSView*`; AppKit has no
cross-process reparent), so the Mac release embeds by running the emulator
inside PiST.

The published `libretro/hatari` tree is not the core we build. Its makefile
still compiles `src/uae-cpu`, the CPU Hatari replaced before 2.6, and the
product is named `hatari2014`. The core is a fork of the Hatari we already pin
— `tattlemuss/hatari` @ `21aa4cb`, upstream 2.6.1 plus `remotedebug.c` — with a
libretro frontend added on that tree.

## What the release job produces

`hatari_libretro.dylib`, linked against `libm` and `libz` only. Under
`__LIBRETRO__` the SDL window, audio device and input grab are stubbed. Frames
leave through the run call below, sound through `pist_hatari_audio`, keys
and mouse through their own calls. No Homebrew library is on the link
line, which is what made a Mac `hatari` executable non-relocatable.

The dylib is built from a checksum-pinned tarball of that fork, the same shape
as the hrdb pin in `.github/actions/build-hatari`. It is copied to
`PiST.app/Contents/Frameworks/hatari_libretro.dylib` before the signature seal.
`otool -L` on it must not mention `/opt/homebrew`. `--diagnose` must name the
core inside the app.

`libretroCoreCandidates()` is the search. From `Contents/MacOS` the first
candidate is `../Frameworks/hatari_libretro.dylib`, then beside the executable.
The same relative Frameworks path is the one the disk image ships.

## The contract

`src/emu/LibretroAbi.h` is the ABI, plain C, versioned by `PIST_HATARI_ABI`.
The fork exports these symbols and no others that PiST depends on. Bump the
constant when a field or a signature changes. The dylib's `pist_hatari_abi()`
must return the same value; a mismatch is a failed start, not a guess.

One thread owns the core. That thread is the only caller of `pist_hatari_run`
and of every debugger entry. The UI thread queues work onto it and draws the
last frame. A frame pointer is valid until the next run or stop. Continue
does not drop a breakpoint arm that is already queued: the owner applies
those arms before it lets the CPU run again.

`pist_hatari_start` takes the session PiST already builds: TOS path, GEMDOS
directory, program path, optional floppies, the Hatari `--machine` name, RAM in
MiB. That is how a PRG is autostarted today. `retro_load_game` on the published
core only understands disk images and a `.gem` directory that boots from
`BOOT.ST`, which is the wrong shape for an IDE.

The ROM is that TOS path and nothing else. Bundled EmuTOS is only what
discovery selects when the user has not chosen an image. A TOS file set in
the project, or any other image the session resolved, is passed through the
same field. The core ships no ROM and does not special-case an EmuTOS
filename. An empty path fails the start.

Debugger calls take the text `IDebugBackend` already produces. A breakpoint
condition is the planner's command (`b pc = $addr`, the watchpoint
self-inequality, `:once` included). The fork feeds that to Hatari's existing
debugger. It does not grow a second condition language. `pist_hatari_ram`
returns ST RAM; the published core's `retro_get_memory_data` returns NULL, and
this symbol is the replacement.

## How PiST drives it

A third `IDebugBackend`, `LibretroBackend`. `createBackend` can construct it.
Session launch selects it when `sessionUsesInProcessCore` is true: macOS, the
dylib is beside the app, and the project has not named its own Hatari. Linux
and Windows stay on native or HRDB. A Mac without the dylib keeps the
subprocess and `brew install hatari`. An explicit debug transport does not
override that: hrdb and native are subprocess channels, and the escape hatch
is the emulator path.

When it is selected, the Emulator panel blits the frame
(`QImage::Format_RGB32`, the layout `PistHatariFrame` documents) letterboxed
the way the Windows embed already fits a foreign window. There is no Hatari
window on that path. Keys and the mouse are forwarded to the core. A Project
Settings emulator path can still name a subprocess Hatari; that session stays
on the existing backends.

`pist_hatari_key` takes an SDL keycode and SDL modifier bits, the values
Hatari's keymap already switches on, and presses or releases that key. The
core clears Hatari's own shortcut table at start, so F11, F12 and Pause
reach the ST. The UI thread posts the call onto the owner thread. A click
focuses the panel; while it is showing a frame, those keys are not PiST
shortcuts.

`pist_hatari_audio` copies queued stereo 16-bit frames at 44100 Hz. The
owner thread pulls that after each frame. The run up to the entry stop stays
fast and silent; sound starts when the user resumes. A session with no
program plays from the first frame, and the owner waits once about 80 ms is
already queued so the speed stays 44100 Hz. A key or a mouse move still runs
during that wait. While the debugger is stopped, playback drains and goes
quiet.

The free-text console, profile save, hardware-info subjects and IPF disks are
not in the first slice. The typed intents are: start, stop, run-until-frame,
pause, step, step-over, resume, arm and clear breakpoints, RAM, registers,
basepage, keys, the mouse, and sound. `command()` for arbitrary debugger text waits until those work.

## Licence

Linking this core into PiST, including by `dlopen`, is a combined work. Hatari
compiles three GPL-2.0-only files, so the combination is conveyed under GPLv2.
PiST is GPL-2.0-or-later, which permits that. The notices name the fork commit
the dylib was built from. The audit and the conveyance rule are `docs/PLAN.md`
§10.

## First slice

1. This contract, and a backend that loads the dylib or reports that it is absent. Done, on `feature/libretro-macos`.
2. The fork's frontend, on branch `pist-libretro` of the pinned Hatari. Bring-up takes the session argv, including `--tos` set to the session path, and does not read `hatari.cfg`. A missing path fails. Two different images come back as the two version words they carry, so a user TOS file replaces EmuTOS by being that path. `pist_hatari_run` returns one frame, or the entry stop (`b pc = TEXT && pc < $e00000 :once`), whichever comes first. Against EmuTOS that is a 640x436 frame and a stop in RAM at the autostarted program. The Linux build still links SDL and forces the dummy video driver. The release dylib, linked against `libm` and `libz` only, is item 4.
3. The panel draws that frame. `LibretroBackend` runs `pist_hatari_run` on the core's owner thread and copies each frame before the next run. `EmulatorDisplayWidget::setFrame` letterboxes it; a frame queued after `stop()` carries an old epoch and is dropped.
4. The macOS release job builds the dylib, seals it into the app, and `--diagnose` finds it. `PIST_LIBRETRO_STUB_SDL` compiles Hatari against `src/pist_sdl` instead of SDL, so the link line is libm and libz (plus the platform libc). On Linux that dylib still returns the 640x436 frame and the entry stop. The job copies `hatari_libretro.dylib` into `Contents/Frameworks` before the signature, refuses an `otool -L` that mentions `/opt/homebrew`, and `--diagnose` prints `Libretro core:` with that path. `ENABLE_OSX_BUNDLE` stays off for this build: the dylib is not a Hatari.app.
5. Session launch selects the backend. On macOS, with the dylib present and an empty Hatari path, Run skips the emulator search, the probe, the control socket, and the bootstrap script, and starts `LibretroBackend`. The core arms the entry breakpoint itself. A named Hatari path, and every Linux and Windows run, stay on the subprocess.
6. The entry stop can continue. `pist_hatari_step`, `pist_hatari_step_over`, `pist_hatari_resume`, `pist_hatari_pause`, `pist_hatari_registers`, `pist_hatari_basepage`, and the arm and clear calls are implemented in the core. `LibretroBackend` runs them on the owner thread. The snapshot those reads return is what arms file:line breakpoints after the bases arrive. The console, profile save, disassembly, hardware info, and history are still later.
7. Keys. The panel takes focus on a click. `qtKeyToSdlSym` turns the Qt key into the SDL keycode Hatari's keymap already maps, and `pist_hatari_key` runs on the owner thread. Hatari's shortcut table is cleared, so a function key is the ST's.
8. Mouse. Motion over the picture is scaled into ST pixels and posted, with the left and right buttons, as `pist_hatari_mouse` on the owner thread. The host cursor is hidden while a frame is showing, because the ST draws its own; the cursor is a transparent pixmap, because `Qt::BlankCursor` on macOS stops move events after a key. The panel does not grab the pointer, and on macOS it is not a native view: either one stops moves after a key. Hover events carry the motion when mouse-move delivery has stopped. Entering the panel does not fling that pointer. Those deltas become ST packets only from the IKBD autosend interrupt. A frame ends by setting Hatari's quit flag, and that interrupt arms itself again anyway, so a frame boundary cannot retire it.
9. Sound. `--sound off` is gone, so Hatari's mixer runs. The stub audio open succeeds and does not open a device; `pist_hatari_audio` is how the samples leave. On macOS, AudioQueue plays them. On Linux the same pull discards them, which keeps the mix ring from wrapping during a test. Playback waits for the user to resume past the entry stop, so that run stays fast. After that, the owner thread is paced by the queue.

Out of that slice: replacing the Linux and Windows subprocess, the console,
profile save, IPF.
