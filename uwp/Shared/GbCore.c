/* See GbCore.h. */
#include <stdlib.h>
#include <string.h>

#include "GbCore.h"
#include "minigb_apu.h"

/* Peanut-GB calls these for the sound registers (0xFF10-0xFF3F). */
uint8_t audio_read(const uint16_t addr);
void audio_write(const uint16_t addr, const uint8_t val);

/* Called by Peanut-GB after every write to the joypad register. */
struct gb_s;
static void on_joyp_write(struct gb_s *gb, uint8_t val);
#define PEANUT_GB_JOYP_WRITE_HOOK(gb, val) on_joyp_write((gb), (val))

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

	/*
	 * Super Game Boy. The game sends 16-byte command packets bit by bit
	 * through the joypad register; bulk data (palettes, border graphics)
	 * is "shown on screen" for the SGB to copy, which we read from VRAM.
	 */
	struct
	{
		/* Packet reception. */
		uint8_t last_joyp;    /* bits 4-5 of the last JOYP write */
		int receiving;
		unsigned bit;
		uint8_t current[16];
		uint8_t command[7 * 16];
		unsigned packets_received;
		unsigned packets_expected;

		/* Multiplayer request (used by the game to detect an SGB). */
		unsigned players;
		unsigned player;

		/* Screen colours. */
		uint16_t system_palettes[512][4]; /* from PAL_TRN */
		uint16_t palettes[4][4];          /* from PAL_SET, BGR555 */
		uint8_t attributes[GBCORE_HEIGHT / 8][GBCORE_WIDTH / 8];
		int coloured;                     /* a PAL_SET has arrived */

		/* Screen mask (MASK_EN). */
		int mask;
		uint8_t frozen[GBCORE_WIDTH * GBCORE_HEIGHT];

		/* Border (CHR_TRN / PCT_TRN), in SNES formats. */
		uint8_t border_tiles[256 * 32];
		uint16_t border_map[32 * 32];
		uint16_t border_palettes[4][16];  /* palettes 4-7 */
		int has_border;
	} sgb;
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

/* ---------------------------------------------------------------------
 * Super Game Boy
 * ------------------------------------------------------------------- */

enum
{
	SGB_ATTR_BLK = 0x04,
	SGB_PAL_SET = 0x0A,
	SGB_PAL_TRN = 0x0B,
	SGB_MLT_REQ = 0x11,
	SGB_CHR_TRN = 0x13,
	SGB_PCT_TRN = 0x14,
	SGB_MASK_EN = 0x17
};

static void sgb_reset(void)
{
	static const uint16_t grey[4] = { 0x7FFF, 0x5294, 0x294A, 0x0000 };
	int i;

	memset(&core.sgb, 0, sizeof(core.sgb));
	core.sgb.last_joyp = 0x30;
	core.sgb.players = 1;
	for(i = 0; i < 4; i++)
		memcpy(core.sgb.palettes[i], grey, sizeof(grey));
}

/*
 * Bulk transfers copy 4 KiB that the game lays out as 256 background
 * tiles shown in reading order on the 20x18 screen.
 */
static void sgb_vram_transfer(uint8_t out[4096])
{
	const uint8_t lcdc = core.gb.hram_io[IO_LCDC];
	const unsigned map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
	unsigned t;

	for(t = 0; t < 256; t++)
	{
		const uint8_t tile =
			core.gb.vram[map + (t / 20) * 32 + (t % 20)];
		const unsigned addr = (lcdc & 0x10) ?
			tile * 16u :
			(unsigned)(0x1000 + (int8_t)tile * 16);
		memcpy(&out[t * 16], &core.gb.vram[addr], 16);
	}
}

static void sgb_attr_block(const uint8_t *d)
{
	unsigned ctrl = d[0] & 7;
	const uint8_t inside = d[1] & 3;
	uint8_t border = (d[1] >> 2) & 3;
	const uint8_t outside = (d[1] >> 4) & 3;
	const unsigned x1 = d[2] & 31, y1 = d[3] & 31;
	const unsigned x2 = d[4] & 31, y2 = d[5] & 31;
	unsigned x, y;

	/* Changing only the inside or only the outside also changes the
	 * border line to the same palette. */
	if(ctrl == 1)
	{
		ctrl |= 2;
		border = inside;
	}
	else if(ctrl == 4)
	{
		ctrl |= 2;
		border = outside;
	}

	for(y = 0; y < GBCORE_HEIGHT / 8; y++)
	{
		for(x = 0; x < GBCORE_WIDTH / 8; x++)
		{
			const int in_rect = x >= x1 && x <= x2 && y >= y1 && y <= y2;
			const int in_inside = x > x1 && x < x2 && y > y1 && y < y2;

			if(in_inside)
			{
				if(ctrl & 1)
					core.sgb.attributes[y][x] = inside;
			}
			else if(in_rect)
			{
				if(ctrl & 2)
					core.sgb.attributes[y][x] = border;
			}
			else if(ctrl & 4)
			{
				core.sgb.attributes[y][x] = outside;
			}
		}
	}
}

static void sgb_run_command(const uint8_t *c, unsigned length)
{
	uint8_t data[4096];
	unsigned i, j;

	switch(c[0] >> 3)
	{
	case SGB_ATTR_BLK:
		for(i = 0; i < c[1] && 2 + i * 6 + 6 <= length; i++)
			sgb_attr_block(&c[2 + i * 6]);
		break;

	case SGB_PAL_SET:
		for(i = 0; i < 4; i++)
		{
			const unsigned id = (c[1 + i * 2] | (c[2 + i * 2] << 8)) & 0x1FF;
			memcpy(core.sgb.palettes[i], core.sgb.system_palettes[id],
					sizeof(core.sgb.palettes[i]));
		}
		if(c[9] & 0x40)
			core.sgb.mask = 0;
		core.sgb.coloured = 1;
		break;

	case SGB_PAL_TRN:
		sgb_vram_transfer(data);
		for(i = 0; i < 512; i++)
			for(j = 0; j < 4; j++)
				core.sgb.system_palettes[i][j] = (uint16_t)
					(data[i * 8 + j * 2] |
					 (data[i * 8 + j * 2 + 1] << 8));
		break;

	case SGB_MLT_REQ:
		switch(c[1] & 3)
		{
		case 0: core.sgb.players = 1; break;
		case 1: core.sgb.players = 2; break;
		default: core.sgb.players = 4; break;
		}
		core.sgb.player = 0;
		break;

	case SGB_CHR_TRN:
		sgb_vram_transfer(data);
		memcpy(&core.sgb.border_tiles[(c[1] & 1) * 4096], data, 4096);
		break;

	case SGB_PCT_TRN:
		sgb_vram_transfer(data);
		for(i = 0; i < 32 * 32; i++)
			core.sgb.border_map[i] =
				(uint16_t)(data[i * 2] | (data[i * 2 + 1] << 8));
		for(i = 0; i < 4; i++)
			for(j = 0; j < 16; j++)
				core.sgb.border_palettes[i][j] = (uint16_t)
					(data[0x800 + (i * 16 + j) * 2] |
					 (data[0x800 + (i * 16 + j) * 2 + 1] << 8));
		core.sgb.has_border = 1;
		break;

	case SGB_MASK_EN:
		if((c[1] & 3) == 1 && core.sgb.mask != 1)
			memcpy(core.sgb.frozen, core.framebuffer,
					sizeof(core.sgb.frozen));
		core.sgb.mask = c[1] & 3;
		break;

	default:
		/* Sound, DATA_SND and the rest are not needed by this game. */
		break;
	}
}

static void sgb_packet_received(void)
{
	if(core.sgb.packets_received == 0)
	{
		core.sgb.packets_expected = core.sgb.current[0] & 7;
		if(core.sgb.packets_expected == 0)
			return;
	}

	memcpy(&core.sgb.command[core.sgb.packets_received * 16],
			core.sgb.current, 16);
	if(++core.sgb.packets_received >= core.sgb.packets_expected)
	{
		sgb_run_command(core.sgb.command, core.sgb.packets_received * 16);
		core.sgb.packets_received = 0;
	}
}

static void on_joyp_write(struct gb_s *gb, uint8_t val)
{
	const uint8_t lines = val & 0x30;
	const uint8_t last = core.sgb.last_joyp;
	core.sgb.last_joyp = lines;

	switch(lines)
	{
	case 0x00:
		/* Both lines low: start of a packet. */
		core.sgb.receiving = 1;
		core.sgb.bit = 0;
		memset(core.sgb.current, 0, sizeof(core.sgb.current));
		break;

	case 0x30:
		/* Both lines high. Raising P15 moves on to the next
		 * controller when several were requested. */
		if(!core.sgb.receiving && !(last & 0x20) && core.sgb.players > 1)
			core.sgb.player = (core.sgb.player + 1) % core.sgb.players;
		break;

	default:
		/* One line low: a data bit (P15 low = 1, P14 low = 0). Bits
		 * are separated by both lines going high again. */
		if(!core.sgb.receiving || last != 0x30)
			break;
		if(core.sgb.bit < 128)
		{
			if(lines == 0x10)
				core.sgb.current[core.sgb.bit >> 3] |=
					(uint8_t)(1u << (core.sgb.bit & 7));
			core.sgb.bit++;
		}
		else
		{
			/* Stop bit. */
			core.sgb.receiving = 0;
			sgb_packet_received();
		}
		break;
	}

	/* With neither row of keys selected, the SGB reports which
	 * controller is being read: 0xF for the first, 0xE for the second. */
	if(lines == 0x30)
		gb->hram_io[IO_JOYP] = (uint8_t)(0xF0 | (0x0F - core.sgb.player));
}

static uint32_t bgr555_to_argb(uint16_t c)
{
	const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
	return 0xFF000000u |
		(((r << 3) | (r >> 2)) << 16) |
		(((g << 3) | (g >> 2)) << 8) |
		((b << 3) | (b >> 2));
}

int gbcore_sgb_coloured(void)
{
	return core.sgb.coloured;
}

int gbcore_sgb_has_border(void)
{
	return core.sgb.has_border;
}

void gbcore_sgb_render(uint32_t *out, int with_border)
{
	const uint8_t *fb = core.sgb.mask == 1 ? core.sgb.frozen : core.framebuffer;
	const unsigned stride = with_border ? GBCORE_SGB_WIDTH : GBCORE_WIDTH;
	const unsigned ox = with_border ? (GBCORE_SGB_WIDTH - GBCORE_WIDTH) / 2 : 0;
	const unsigned oy = with_border ? (GBCORE_SGB_HEIGHT - GBCORE_HEIGHT) / 2 : 0;
	/* Colour 0 of palette 0 is shared by every palette. */
	const uint32_t backdrop = bgr555_to_argb(core.sgb.palettes[0][0]);
	uint32_t colours[4][4];
	unsigned x, y, i, j;

	for(i = 0; i < 4; i++)
		for(j = 0; j < 4; j++)
			colours[i][j] = j == 0 ? backdrop :
				bgr555_to_argb(core.sgb.palettes[i][j]);

	if(with_border)
		for(i = 0; i < GBCORE_SGB_WIDTH * GBCORE_SGB_HEIGHT; i++)
			out[i] = backdrop;

	for(y = 0; y < GBCORE_HEIGHT; y++)
	{
		uint32_t *row = &out[(oy + y) * stride + ox];
		for(x = 0; x < GBCORE_WIDTH; x++)
		{
			switch(core.sgb.mask)
			{
			case 2: row[x] = 0xFF000000u; break;
			case 3: row[x] = backdrop; break;
			default:
				row[x] = colours[core.sgb.attributes[y / 8][x / 8]]
					[fb[y * GBCORE_WIDTH + x] & 3];
				break;
			}
		}
	}

	if(!with_border || !core.sgb.has_border)
		return;

	/* The border is a 32x28 map of 8x8, 16-colour SNES tiles. Colour 0
	 * is transparent, which leaves the game screen showing through. */
	for(y = 0; y < GBCORE_SGB_HEIGHT / 8; y++)
	{
		for(x = 0; x < GBCORE_SGB_WIDTH / 8; x++)
		{
			const uint16_t e = core.sgb.border_map[y * 32 + x];
			const uint8_t *tile = &core.sgb.border_tiles[(e & 0xFF) * 32];
			const unsigned pal = ((e >> 10) & 7) >= 4 ? ((e >> 10) & 7) - 4 : 0;
			unsigned py, px;

			for(py = 0; py < 8; py++)
			{
				const unsigned r = (e & 0x8000) ? 7 - py : py;
				const uint8_t p0 = tile[r * 2], p1 = tile[r * 2 + 1];
				const uint8_t p2 = tile[16 + r * 2], p3 = tile[16 + r * 2 + 1];
				uint32_t *dst = &out[(y * 8 + py) * GBCORE_SGB_WIDTH + x * 8];

				for(px = 0; px < 8; px++)
				{
					const unsigned b = (e & 0x4000) ? px : 7 - px;
					const unsigned ci = ((p0 >> b) & 1) |
						(((p1 >> b) & 1) << 1) |
						(((p2 >> b) & 1) << 2) |
						(((p3 >> b) & 1) << 3);
					if(ci != 0)
						dst[px] = bgr555_to_argb(
							core.sgb.border_palettes[pal][ci]);
				}
			}
		}
	}
}

/* --------------------------------------------------------------------- */

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
	sgb_reset();
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
	sgb_reset();
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
