# Pokémon Red & Blue for Xbox (UWP)

This folder builds the ROMs from this repository into **two separate UWP apps**
that run on an Xbox in Developer Mode. They also run on Windows 10 and 11.

| App          | Package name               | ROM it embeds  | Save file      |
|--------------|----------------------------|----------------|----------------|
| Pokémon Red  | `PokeredUWP.PokemonRed`    | `pokered.gbc`  | `pokered.sav`  |
| Pokémon Blue | `PokeredUWP.PokemonBlue`   | `pokeblue.gbc` | `pokeblue.sav` |

Each app has its own tile, colours and save data, so you can install both side
by side. Inside each app, the game runs on the
[Peanut-GB](https://github.com/deltabeard/Peanut-GB) emulator core with the
minigb_apu sound core (both MIT licensed, in `External/PeanutGB`). The app is a
C++/WinRT `CoreWindow` app that draws with Direct2D and plays audio with
XAudio2.

> Each package contains the game ROM. Keep this repository, its Actions
> artifacts and the packages private.

## Getting the packages

### From GitHub Actions (no Windows PC needed)

Every push runs the **UWP (Xbox)** workflow (`.github/workflows/uwp.yml`):

1. A Linux job builds `pokered.gbc` and `pokeblue.gbc` with RGBDS.
2. A Windows job packages each game and signs it with a self-signed certificate.

Open the workflow run and download the **PokemonRed-Xbox** and
**PokemonBlue-Xbox** artifacts. Each one contains:

- `PokemonRed_1.0.<run>.0_x64.msix`: the app.
- `Dependencies/x64/Microsoft.VCLibs.x64.14.00.appx`: the C++ runtime it needs.
- A `.cer` certificate (only needed to install on a Windows PC).

By default each run makes a new throwaway certificate. To install a new build
as an update over an old one and keep its save, sign every build with the same
certificate. To do that, add two repository secrets:

- `SIGNING_CERT_PFX_BASE64`: a base64-encoded `.pfx` with subject `CN=PokeredUWP`.
  `uwp/build.ps1` creates one in `uwp/cert/`.
- `SIGNING_CERT_PASSWORD`: the password for that `.pfx`.

### On a Windows PC

You need Visual Studio 2022 with the **Universal Windows Platform development**
workload (include the C++ UWP tools).

1. Build the ROMs as described in [INSTALL.md](../INSTALL.md). Under WSL, run
   `make red blue` in the repository root.
2. From the repository root, run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File uwp\build.ps1             # both games
   powershell -ExecutionPolicy Bypass -File uwp\build.ps1 -Game Blue  # just one
   ```

   The packages are written to `uwp\dist\PokemonRed` and `uwp\dist\PokemonBlue`.

Or open `uwp\PokemonUWP.sln`, choose **Release | x64**, set the target to
**Remote Machine** (your Xbox's IP address) and press F5. Visual Studio
deploys the app straight to the console.

## Installing on Xbox

1. Put the console in [Developer Mode](https://learn.microsoft.com/windows/uwp/xbox-apps/devkit-activation).
2. In Dev Home, turn on **Remote Access**. Then open the Device Portal at
   `https://<xbox-ip>:11443` in a browser on your PC.
3. Under **Home → My games & apps**, choose **Add**. Pick the `.msix` and select
   **Next**. Add `Microsoft.VCLibs.x64.14.00.appx` from `Dependencies/x64` as a
   dependency, then select **Start**.
4. Repeat for the other game.
5. **Recommended:** in Dev Home, highlight each app, press View, choose
   **View details** and set **App type** to **Game**. This gives the app game
   resources instead of the smaller app allowance.

## Controls

| Game Boy      | Xbox controller             | Keyboard                    |
|---------------|-----------------------------|-----------------------------|
| D-pad         | D-pad or left stick         | Arrow keys or WASD          |
| A             | A                           | X or K                      |
| B             | B or X                      | Z or J                      |
| Start         | Menu                        | Enter                       |
| Select        | View                        | Backspace or Shift          |
| Fast-forward  | Hold right trigger (4×)     | Hold Tab                    |
| Colour scheme | Y                           | P                           |
| Scaling       | Click left stick            | F                           |

There are four colour schemes: the cartridge's Game Boy Color colours (red or
blue), the original Game Boy green, Game Boy Pocket, and greyscale. Scaling
switches between pixel-perfect (integer) and fill-the-screen. The app remembers
both settings.

## Saves

When you save in the game, the cartridge's battery RAM is written to the app's
`LocalState` folder within about a second. It is also written when the app is
suspended or closed. These are normal 32 KiB `.sav` files, so you can move them
to and from other emulators. On Xbox, use the Device Portal's **File explorer**:
`LocalAppData\PokeredUWP.PokemonRed_<id>\LocalState`.

## Layout

```
uwp/
  PokemonUWP.sln             Both apps
  PokemonRed/                Red: project, manifest, tile art
  PokemonBlue/               Blue: project, manifest, tile art
  Shared/
    App.cpp                  CoreWindow app: video, audio, input, saves
    GbCore.c/.h              C wrapper around the emulator core
    GameConfig.h             Per-game title, save name and palettes
    GameApp.props/.targets   Shared MSBuild settings; copies the ROM in as Game.gb
  External/PeanutGB/         Vendored emulator and APU cores (MIT)
  art/                       Cover art the tiles and splash screens are made from
  tools/make_assets.py       Regenerates the tile and splash images from art/
  build.ps1                  Builds and signs the packages
```
