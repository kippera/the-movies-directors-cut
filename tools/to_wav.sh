#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# to_wav.sh - convert any number of audio (or video) files to the format The Movies wants for "Movie Sounds":
#             WAV, 16-bit PCM, 44.1 kHz, mono, with a tidy lowercase_underscore name.
#
# Use:   drag files onto "Convert to Movie WAV" (the .desktop launcher next to this script), or
#        tools/to_wav.sh [-d OUTDIR] file1 file2 ...
# Output goes to OUTDIR if given, otherwise to a folder "movie_wav" next to the first input file.
# Existing files are never overwritten: a clash gets a numeric suffix (name_2.wav).
# Needs ffmpeg. Copy the finished .wav files into  Documents/The Movies/Movie Sounds  (inside the game's prefix).

OUT=""
if [ "$1" = "-d" ]; then OUT="$2"; shift 2; fi
[ $# -eq 0 ] && { echo "usage: $0 [-d OUTDIR] file..."; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found"; exit 1; }
[ -z "$OUT" ] && OUT="$(dirname "$1")/movie_wav"
mkdir -p "$OUT" || exit 1

slug() { # file name without extension -> lowercase ascii with underscores
    local s
    s=$(printf '%s' "$1" | iconv -f utf-8 -t ascii//TRANSLIT 2>/dev/null | tr 'A-Z' 'a-z' | sed -E 's/[^a-z0-9]+/_/g; s/^_+|_+$//g')
    [ -z "$s" ] && s="sound"
    printf '%s' "${s:0:60}"
}

ok=0; bad=0; made=()
for f in "$@"; do
    [ -f "$f" ] || { echo "skip (not a file): $f"; bad=$((bad+1)); continue; }
    base=$(basename "$f"); name=$(slug "${base%.*}")
    dest="$OUT/$name.wav"; n=2
    while [ -e "$dest" ]; do dest="$OUT/${name}_$n.wav"; n=$((n+1)); done
    if ffmpeg -v error -y -i "$f" -vn -ac 1 -ar 44100 -c:a pcm_s16le "$dest" </dev/null; then
        echo "ok   $base -> $(basename "$dest")"; ok=$((ok+1)); made+=("$dest")
    else
        echo "FAIL $base"; rm -f "$dest"; bad=$((bad+1))
    fi
done
echo "done: $ok converted, $bad failed -> $OUT"
msg="$ok converted, $bad failed
$OUT"
if command -v notify-send >/dev/null; then notify-send -i audio-x-generic "Convert to Movie WAV" "$msg"; fi
[ $bad -eq 0 ]
