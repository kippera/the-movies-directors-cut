#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# removes the debug / automation leftovers the proxy and the test tools write into the game folder (never touches the game's own files, the ini, dxvk.conf or the DLLs)
G="${1:?usage: clean_game_dir.sh \"<Game Files folder>\"}"
pgrep -x MoviesSE.exe >/dev/null && { echo "game running, not cleaning"; exit 1; }
cd "$G" && rm -f TheMoviesGfx.*.bmp TheMoviesGfx.*.raw TheMoviesGfx.*.pgm TheMoviesGfx.files.log TheMoviesGfx.mem.txt TheMoviesGfx.probe.log TheMoviesGfx.vtscan.txt TheMoviesGfx.widgets.csv TheMoviesGfx.cursor.on TheMoviesGfx.cursor.txt TheMoviesGfx.*.now d3d9.dll.new && echo cleaned
