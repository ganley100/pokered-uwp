// Per-game settings. Each app project defines exactly one of POKE_GAME_RED or
// POKE_GAME_BLUE (see PokemonRed/PokemonRed.vcxproj, PokemonBlue/PokemonBlue.vcxproj).
#pragma once

#include <cstdint>

namespace GameConfig
{
	// Colours are 0xRRGGBB, lightest shade first.
	struct Palette
	{
		const wchar_t *name;
		uint32_t bg[4];   // background and window
		uint32_t obj0[4]; // sprites using OBP0
		uint32_t obj1[4]; // sprites using OBP1
	};

#define POKE_SHADES(a, b, c, d) { a, b, c, d }
#define POKE_MONO(a, b, c, d) POKE_SHADES(a, b, c, d), POKE_SHADES(a, b, c, d), POKE_SHADES(a, b, c, d)

#if defined(POKE_GAME_RED) && !defined(POKE_GAME_BLUE)
	constexpr const wchar_t *kTitle = L"Pokémon Red";
	constexpr const wchar_t *kSaveFile = L"pokered.sav";
	// Matches the colours a Game Boy Color picks for this cartridge.
	constexpr Palette kGamePalette = {
		L"Red",
		POKE_SHADES(0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000),
		POKE_SHADES(0xFFFFFF, 0x7BFF31, 0x008400, 0x000000),
		POKE_SHADES(0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000),
	};
#elif defined(POKE_GAME_BLUE) && !defined(POKE_GAME_RED)
	constexpr const wchar_t *kTitle = L"Pokémon Blue";
	constexpr const wchar_t *kSaveFile = L"pokeblue.sav";
	constexpr Palette kGamePalette = {
		L"Blue",
		POKE_SHADES(0xFFFFFF, 0x63A5FF, 0x0000FF, 0x000000),
		POKE_SHADES(0xFFFFFF, 0xFF8484, 0x943A3A, 0x000000),
		POKE_SHADES(0xFFFFFF, 0x63A5FF, 0x0000FF, 0x000000),
	};
#else
#error Define exactly one of POKE_GAME_RED or POKE_GAME_BLUE.
#endif

	// Cycled with the Y button (or P on a keyboard).
	constexpr Palette kPalettes[] = {
		kGamePalette,
		{ L"Original Game Boy", POKE_MONO(0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F) },
		{ L"Game Boy Pocket", POKE_MONO(0xE0F8D0, 0x88C070, 0x346856, 0x081820) },
		{ L"Greyscale", POKE_MONO(0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000) },
	};
	constexpr int kPaletteCount = static_cast<int>(sizeof(kPalettes) / sizeof(kPalettes[0]));

#undef POKE_MONO
#undef POKE_SHADES
}
