/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PiST - an IDE for Atari ST assembly development
 *
 * The C ABI of hatari_libretro, the in-process core the macOS app embeds.
 * The Hatari fork exports these symbols. PiST loads them. The two copies of
 * this header must stay identical; PIST_HATARI_ABI is how a mismatch is
 * refused. The plan is docs/agents/mac.md.
 */

#ifndef PIST_LIBRETRO_ABI_H
#define PIST_LIBRETRO_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when a field or a function signature changes. */
#define PIST_HATARI_ABI 6

/* Pixels are QImage::Format_RGB32: a native 32-bit word 0xFFRRGGBB.
 * `pixels` is valid until the next pist_hatari_run or pist_hatari_stop.
 * `pitch` is bytes per row. */
typedef struct PistHatariFrame {
    const uint8_t *pixels;
    int width;
    int height;
    int pitch;
} PistHatariFrame;

/* The session PiST already builds for a subprocess launch. Strings are
 * UTF-8, NUL-terminated, and may be NULL when that piece is absent.
 * `machine` is Hatari's --machine name (st, ste, megast, megaste, tt, falcon).
 * `monitor` is Hatari's --monitor name: mono, rgb, vga or tv. NULL is mono. */
typedef struct PistHatariSession {
    int abi;
    const char *tosPath;
    const char *gemdosDir;
    const char *programPath;
    const char *diskA;
    const char *diskB;
    const char *machine;
    const char *monitor;
    int memSizeMiB;
} PistHatariSession;

/* The value of PIST_HATARI_ABI this dylib was built against. */
int pist_hatari_abi(void);

/* 0 on success. On failure, if err is non-NULL, a NUL-terminated message is
 * written into err[0 .. errCap). */
int pist_hatari_start(const PistHatariSession *session, char *err, int errCap);
void pist_hatari_stop(void);

/* Advance the machine. One thread, the owner, is the only legal caller.
 * *stopped is set to 1 when the CPU is in the debugger afterwards.
 * *frame is filled when a frame was produced; otherwise pixels is NULL.
 * 0 on success. */
int pist_hatari_run(PistHatariFrame *frame, int *stopped);

/* ST RAM. *size is the length in bytes. NULL if the machine is not up. */
void *pist_hatari_ram(size_t *size);

/* Register snapshot. names[i] and values[i] are parallel. Returns the count
 * written, or -1 if valuesCap is too small (the required count is then
 * written to *needed when needed is non-NULL). */
int pist_hatari_registers(const char **names, uint32_t *values, int valuesCap, int *needed);

/* Text and data and bss bases, the live addresses a source line resolves
 * against. 0 on success. */
int pist_hatari_basepage(uint32_t *textBase, uint32_t *dataBase, uint32_t *bssBase);

int pist_hatari_pause(void);
int pist_hatari_step(void);
int pist_hatari_step_over(void);
int pist_hatari_resume(void);

/* `condition` is the breakpoint command IDebugBackend::armBreakpoint already
 * sends, including the leading "b ". */
int pist_hatari_arm_breakpoint(const char *condition);
int pist_hatari_clear_breakpoints(void);

/* Press or release one host key. `sym` is an SDL_Keycode and `mod` is
 * SDL_Keymod: the values Hatari's keymap already switches on. `down` is 1
 * for a press and 0 for a release. The owner thread is the only legal
 * caller. 0 when the machine is up. */
int pist_hatari_key(int sym, int mod, int down);

/* Relative motion in ST pixels, and which buttons are held. Bit 0 is the
 * left button, bit 1 the right. The owner thread is the only legal caller.
 * 0 when the machine is up. */
int pist_hatari_mouse(int dx, int dy, int buttons);

/* Stereo signed 16-bit host-endian frames, 44100 Hz, interleaved left,
 * right. Copies up to `frames` frames into `interleaved` and returns how
 * many were copied. 0 when sound is off, nothing is queued, or the machine
 * is down. The owner thread is the only legal caller. */
int pist_hatari_audio(int16_t *interleaved, int frames);

/* Run one Hatari debugger line. Copies the text it printed into `out`,
 * NUL-terminated. `*needed` is that text's length, not counting the NUL,
 * even when `out` is too small. `out` may be NULL.
 * 0 when the line finished and the machine is still in the state it was in.
 * 2 when the line left the debugger: the machine is running, and the next
 * pist_hatari_run advances it. A step or a step-over is this too; the core
 * stops again from inside that run.
 * 1 when the machine is down, the line is empty, or the text could not be
 * captured. The owner thread is the only legal caller. */
int pist_hatari_command(const char *line, char *out, int outCap, int *needed);

#ifdef __cplusplus
}
#endif

#endif
