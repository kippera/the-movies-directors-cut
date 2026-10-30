# Notices

## This project
Copyright (C) 2026 the authors of The Movies: Directors Cut. Licensed under the GNU General Public License, version 3 or (at your option) any later version. See `LICENSE`.

## Third-party components linked into the released binaries
* **FFmpeg 6.1.1** (libavformat, libavcodec, libswscale, libswresample, libavutil), https://ffmpeg.org. Built with `--enable-gpl --enable-libx264`, so the combined work is GPL. Configure flags: `build/ffmpeg-build.sh` and `build/build_mp4_ffmpeg.sh`. FFmpeg's own licence texts are in its source distribution.
* **x264**, https://www.videolan.org/developers/x264.html, GPL-2.0-or-later, statically linked into `asfreader.dll`.
* **mingw-w64** runtime and winpthreads, statically linked (permissive licences with the GCC runtime library exception).

## Tools used but not shipped
Wine's d3dcompiler (to precompile the pixel shaders in `gfx/shader*.h`), GE-Proton, DXVK.

## The game
"The Movies" and "The Movies: Stunts & Effects" are trademarks and copyrighted works of their respective owners (Activision, Lionhead Studios). This project is an unofficial fan patch. The release zips include edited copies of some of the game's data files (Flash UI files and export profiles); they are only useful to people who own the game.
