#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# writes loose export profiles into Data/Video (the game and wmvcore pick them up in place of the pak copies) for the modern presets.
# level: Upload 428x240 1 Mbps / Small 640x360 2 Mbps / Medium 854x480 4 Mbps / High 1280x720 10 Mbps / Best 1920x1080 20 Mbps (wmv2 needs more bits than wmv3)
# usage: make_presets.sh "<Game Files folder>" "<folder with the stock wmvprofile_*.prx, extracted with re/pakx.py>"
G="${1:?usage: make_presets.sh \"<Game Files folder>\" \"<stock prx folder>\"}"; S="${2:?stock prx folder missing}"; M="$(dirname "$0")/mkprofile.py"
mkdir -p "$G/Data/Video"
mk(){ for suf in "" _na; do python3 $M $S/wmvprofile_$1$suf.prx "$G/Data/Video/wmvprofile_$1$suf.prx" $2 $3 $4 "The Movies WMV Profile ($2 x $3) $5"; done; }
mk upload 428 240 1000000 Upload
mk small  640 360 2000000 Low
mk medium 854 480 4000000 Medium
mk high   1280 720 10000000 High
mk best   1920 1080 20000000 Highest
