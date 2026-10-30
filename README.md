# The Movies: Directors Cut

A fan-made patch set for **The Movies: Stunts & Effects**: a sharper movie viewer, real export sizes, MP4 export, widescreen UI touches and colour settings. It was built and tested on **GE-Proton 10-33** (Linux), where it works out of the box with no winetricks. A Windows build is available as an **experimental** release.

Not affiliated with or endorsed by Activision or Lionhead Studios. "The Movies" belongs to its owners. This repository contains no game assets; the release zips contain edited copies of two kinds of the game's data files (see [Releases](#releases)) and need the game to be useful.

## What it does

| | |
|---|---|
| **Sharper viewer** | Movie playback renders at 1024 or 2048 pixels instead of 512 (`MovieRes`), with layering and 2D cards kept correct. |
| **Modern export sizes** | The five quality levels export at their real sizes: Upload 428x240, Low 640x360, Medium 854x480, High 1280x720, Best 1920x1080. |
| **Linux fixes** | MP4 export (H.264 + AAC, `ExportCRF` 14-30), a built-in Windows Media replacement so intro/menu videos and export work, and an export that no longer freezes at "Saving Sound Data" (its own Wav Parser instead of Wine's GStreamer one). Includes a drag-and-drop sound converter. |
| **Colour settings** | Brightness, contrast, gamma, saturation, FXAA, sharpen and MSAA. In game, `Ctrl+Shift+F7` opens the settings panel. |
| **Widescreen fixes** | Edited Flash UI files keep the round HUD buttons round at 16:9 and widen the pause, Movie Maker and menu screens. Clipped letter tails in small text are repaired (`TextDescent`). |

## Install

### Linux / Proton (tested)

1. Download the Linux zip from the latest release.
2. Copy everything except `tools` and `source` into the folder that contains `MoviesSE.exe`, merging the `Data` folder.
3. Add this Steam launch option: `WINEDLLOVERRIDES="*wmvcore=n,b;d3d9=n,b" %command%`
4. Set the game's video options to your screen size once. Do not pick a size the game does not list.

To remove it, delete the copied files and the launch option.

### Windows (experimental)

1. Download the Windows zip from the latest release and close the game.
2. Back up the ten files in `Data\Video`, then copy the zip's contents into `Game Files`, merging `Data`.
3. Start the game. If it will not start, delete `d3d9.dll`.

The Windows build contains only the graphics proxy, the export presets and the widescreen UI files. MP4 export and the Wav Parser replacement are Linux-only; Windows exports WMV as the stock game does.

## Settings

`TheMoviesGfx.ini` sits next to `MoviesSE.exe` and is reloaded while the game runs.

| Key | Values | Effect |
|---|---|---|
| `MovieRes` | 512, 1024, 2048 | Movie playback resolution (next start) |
| `ExportPresets` | 0, 1 | Real sizes for the five export levels |
| `ExportFormat` / `ExportCRF` | mp4, wmv / 14-30 | Linux build: export type and H.264 quality |
| `Brightness`, `Contrast`, `Gamma`, `Saturation` | 0, 1, 1, 1 | Colour grading, live |
| `FXAA`, `Sharpen`, `MSAA` | 0/1, 0-1, 0/2/4/8 | Edge smoothing; MSAA needs a restart |
| `TextDescent` | 0, 1 | Repair cut-off letter tails |
| `Log` | 0, 1 | Write `TheMoviesGfx.log` (frame-time and timing lines) |

## Tools

* `tools/to_wav.sh` and `tools/Convert to Movie WAV.desktop`: drop any audio files on it to get 16-bit, 44.1 kHz mono WAVs for `Movie Sounds` (edit the `Exec` path in the `.desktop` file).
* `tools/gfx_settings.py "<Game Files folder>"`: an optional external settings window (Python 3 + PyQt6).
* `tools/make_presets.sh`, `tools/mkprofile.py`: write the export profiles from the game's stock ones (`re/pakx.py` extracts those from the game's `.pak` files).
* `mfl/`: tools that produced the edited Flash UI files (`mflsqueeze.py` and friends).

## Building

Everything is 32-bit Windows code cross-built with `i686-w64-mingw32-gcc`.

```sh
# graphics proxy (shaders are pre-generated in shader.h / shader_fx.h)
cd gfx
i686-w64-mingw32-gcc -O2 -mstackrealign -shared -o d3d9.dll d3d9proxy.c d3d9proxy.def \
    -Wl,--kill-at -static -static-libgcc -lgdi32 -luser32

# Windows Media replacement: needs ffmpeg 6.1.1 + x264, see build/build_mp4_ffmpeg.sh
FFMPEG_TARBALL=/path/to/ffmpeg-6.1.1.tar.xz build/build_mp4_ffmpeg.sh
cd shim
i686-w64-mingw32-gcc -O2 -mstackrealign -shared -o asfreader.dll asfreader.c asfwriter.c filewriter.c \
    devenum.c wavparse.c asfreader.def -Wl,--kill-at -L<ffout>/lib -L<x264out>/lib \
    -lavformat -lavcodec -lswscale -lswresample -lavutil -lx264 -lstrmiids -luuid -lole32 -loleaut32 \
    -lbcrypt -lws2_32 -lsecur32 -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
```

`docs/DEVELOPMENT_NOTES.md` is the full engineering log: how the game was reverse engineered, why each fix exists, test methods and dead ends.

## Releases

Each release has two zips and their MD5 sums:

* `TheMoviesDirectorsCut-Linux-Proton.zip`
* `TheMoviesDirectorsCut-Windows-experimental.zip`

Besides the built DLLs, the zips contain `Data/ui/flash/*.mfl` (edited copies of the game's Flash UI files) and `Data/Video/*.prx` (export profiles derived from the game's own). Both are modified game data and only useful with a copy of the game. The tools that produced them are in `mfl/` and `tools/`.
## AI assistance

This project was built with assistance from Claude Code, which wrote much of the code. Everything has been heavily monitored and directed by myself, including all current testing.

## Licence

GPL-3.0-or-later; see `LICENSE` and `NOTICE.md`. The shim is statically linked with x264 (GPL) and FFmpeg, so the DLLs as released are GPL and this repository is their complete source.
