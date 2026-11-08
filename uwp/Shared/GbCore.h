/*
 * GbCore: a small C wrapper around the Peanut-GB CPU/PPU core and the
 * minigb_apu sound core. It owns exactly one emulated Game Boy.
 *
 * Build requirements (set in Shared/Common.props):
 *   MINIGB_APU_AUDIO_FORMAT_S16SYS  - 16-bit signed audio samples
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBCORE_WIDTH  160
#define GBCORE_HEIGHT 144

/* Button bits for gbcore_set_buttons(). A set bit means "held down". */
#define GBCORE_BTN_A      0x01
#define GBCORE_BTN_B      0x02
#define GBCORE_BTN_SELECT 0x04
#define GBCORE_BTN_START  0x08
#define GBCORE_BTN_RIGHT  0x10
#define GBCORE_BTN_LEFT   0x20
#define GBCORE_BTN_UP     0x40
#define GBCORE_BTN_DOWN   0x80

typedef enum GbCoreResult
{
	GBCORE_OK = 0,
	GBCORE_ERR_UNSUPPORTED_CART,
	GBCORE_ERR_BAD_CHECKSUM,
	GBCORE_ERR_NO_MEMORY,
	GBCORE_ERR_BAD_ROM
} GbCoreResult;

/*
 * Start emulating `rom`. The ROM bytes are copied, so the caller may free them.
 * Cartridge RAM (the save file) starts zeroed; load one with gbcore_save_data().
 */
GbCoreResult gbcore_init(const uint8_t *rom, size_t rom_size);
void gbcore_shutdown(void);

/* Power-cycle the console. Cartridge RAM is kept. */
void gbcore_reset(void);

/* Battery-backed cartridge RAM. Size is 0 if the cartridge has none. */
size_t gbcore_save_size(void);
uint8_t *gbcore_save_data(void);
/* Non-zero if the game wrote to cartridge RAM since the last clear. */
int gbcore_save_dirty(void);
void gbcore_clear_save_dirty(void);

void gbcore_set_buttons(uint8_t held);

/* Run exactly one video frame (70224 CPU cycles) and its audio. */
void gbcore_run_frame(void);

/*
 * GBCORE_WIDTH * GBCORE_HEIGHT bytes. Bits 0-1 are the shade (0 = lightest),
 * bits 4-5 the layer: 0 = OBJ0, 1 = OBJ1, 2 = BG/window.
 */
const uint8_t *gbcore_framebuffer(void);

/* Audio produced by the last gbcore_run_frame(): stereo, interleaved int16. */
unsigned gbcore_audio_sample_rate(void);
unsigned gbcore_audio_frames_per_video_frame(void); /* stereo frames */
const int16_t *gbcore_audio_buffer(void);

/* The 16-character title from the cartridge header (NUL-terminated). */
const char *gbcore_rom_title(void);

/* Non-zero once the core hit an unrecoverable CPU error (invalid opcode). */
int gbcore_crashed(void);

#ifdef __cplusplus
}
#endif
