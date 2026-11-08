/* See GbCore.h. */
#include <stdlib.h>
#include <string.h>

#include "GbCore.h"
#include "minigb_apu.h"

/* Peanut-GB calls these for the sound registers (0xFF10-0xFF3F). */
uint8_t audio_read(const uint16_t addr);
void audio_write(const uint16_t addr, const uint8_t val);

#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#include "peanut_gb.h"

#define GBCORE_AUDIO_BUFFER_SAMPLES 4096

static struct
{
	struct gb_s gb;
	struct minigb_apu_ctx apu;

	uint8_t *rom;
	size_t rom_size;

	uint8_t *cart_ram;
	size_t cart_ram_size;
	int cart_ram_dirty;

	uint8_t framebuffer[GBCORE_WIDTH * GBCORE_HEIGHT];
	/* AUDIO_SAMPLES_TOTAL is not an integer constant expression (it is
	 * computed from floating-point values), so reserve a fixed buffer
	 * and check it is large enough in gbcore_init(). */
	audio_sample_t audio[GBCORE_AUDIO_BUFFER_SAMPLES];

	char title[17];
	int crashed;
	int initialised;
} core;

uint8_t audio_read(const uint16_t addr)
{
	return minigb_apu_audio_read(&core.apu, addr);
}

void audio_write(const uint16_t addr, const uint8_t val)
{
	minigb_apu_audio_write(&core.apu, addr, val);
}

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
	(void)gb;
	return addr < core.rom_size ? core.rom[addr] : 0xFF;
}

static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
	(void)gb;
	return addr < core.cart_ram_size ? core.cart_ram[addr] : 0xFF;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
		const uint8_t val)
{
	(void)gb;
	if(addr >= core.cart_ram_size)
		return;

	if(core.cart_ram[addr] != val)
	{
		core.cart_ram[addr] = val;
		core.cart_ram_dirty = 1;
	}
}

static void on_error(struct gb_s *gb, const enum gb_error_e err,
		const uint16_t addr)
{
	(void)gb;
	(void)addr;
	/* Invalid reads/writes are harmless on real hardware; only an invalid
	 * opcode locks the CPU up. */
	if(err == GB_INVALID_OPCODE)
		core.crashed = 1;
}

static void draw_line(struct gb_s *gb, const uint8_t *pixels,
		const uint_fast8_t line)
{
	(void)gb;
	if(line < GBCORE_HEIGHT)
		memcpy(&core.framebuffer[line * GBCORE_WIDTH], pixels,
				GBCORE_WIDTH);
}

GbCoreResult gbcore_init(const uint8_t *rom, size_t rom_size)
{
	enum gb_init_error_e err;
	size_t ram_size = 0;

	gbcore_shutdown();

	if(AUDIO_SAMPLES_TOTAL > GBCORE_AUDIO_BUFFER_SAMPLES)
		return GBCORE_ERR_NO_MEMORY;

	/* Smallest valid cartridge is 32 KiB; header ends at 0x150. */
	if(rom == NULL || rom_size < 0x8000)
		return GBCORE_ERR_BAD_ROM;

	core.rom = (uint8_t *)malloc(rom_size);
	if(core.rom == NULL)
		return GBCORE_ERR_NO_MEMORY;
	memcpy(core.rom, rom, rom_size);
	core.rom_size = rom_size;

	err = gb_init(&core.gb, rom_read, cart_ram_read, cart_ram_write,
			on_error, NULL);
	switch(err)
	{
	case GB_INIT_NO_ERROR:
		break;
	case GB_INIT_CARTRIDGE_UNSUPPORTED:
		gbcore_shutdown();
		return GBCORE_ERR_UNSUPPORTED_CART;
	case GB_INIT_INVALID_CHECKSUM:
		gbcore_shutdown();
		return GBCORE_ERR_BAD_CHECKSUM;
	default:
		gbcore_shutdown();
		return GBCORE_ERR_BAD_ROM;
	}

	if(gb_get_save_size_s(&core.gb, &ram_size) != 0)
		ram_size = 0;
	if(ram_size > 0)
	{
		core.cart_ram = (uint8_t *)calloc(1, ram_size);
		if(core.cart_ram == NULL)
		{
			gbcore_shutdown();
			return GBCORE_ERR_NO_MEMORY;
		}
	}
	core.cart_ram_size = ram_size;

	gb_init_lcd(&core.gb, draw_line);
	minigb_apu_audio_init(&core.apu);
	gb_get_rom_name(&core.gb, core.title);

	core.initialised = 1;
	return GBCORE_OK;
}

void gbcore_shutdown(void)
{
	free(core.rom);
	free(core.cart_ram);
	memset(&core, 0, sizeof(core));
}

void gbcore_reset(void)
{
	if(!core.initialised)
		return;
	gb_reset(&core.gb);
	minigb_apu_audio_init(&core.apu);
	core.crashed = 0;
}

size_t gbcore_save_size(void)
{
	return core.cart_ram_size;
}

uint8_t *gbcore_save_data(void)
{
	return core.cart_ram;
}

int gbcore_save_dirty(void)
{
	return core.cart_ram_dirty;
}

void gbcore_clear_save_dirty(void)
{
	core.cart_ram_dirty = 0;
}

void gbcore_set_buttons(uint8_t held)
{
	/* Peanut-GB's joypad register is active-low. The GBCORE_BTN_* bits
	 * use the same layout as Peanut-GB's JOYPAD_* bits. */
	core.gb.direct.joypad = (uint8_t)~held;
}

void gbcore_run_frame(void)
{
	if(!core.initialised)
		return;

	if(core.crashed)
	{
		memset(core.audio, 0, sizeof(core.audio));
		return;
	}

	gb_run_frame(&core.gb);
	minigb_apu_audio_callback(&core.apu, core.audio);
}

const uint8_t *gbcore_framebuffer(void)
{
	return core.framebuffer;
}

unsigned gbcore_audio_sample_rate(void)
{
	return AUDIO_SAMPLE_RATE;
}

unsigned gbcore_audio_frames_per_video_frame(void)
{
	return AUDIO_SAMPLES;
}

const int16_t *gbcore_audio_buffer(void)
{
	return core.audio;
}

const char *gbcore_rom_title(void)
{
	return core.title;
}

int gbcore_crashed(void)
{
	return core.crashed;
}
