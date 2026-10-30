#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds x264 (H.264 encoder) and a SECOND ffmpeg (with libx264, aac and the mp4 muxer) for the 32-bit Windows shim.
# Everything goes to $W (default /tmp/claude-1000/mp4); the project's own ff/out is NOT touched.
# x264 is GPL: a shim linked against it is GPL too (the package ships its source).
set -e
W="${W:-/tmp/claude-1000/mp4}"
mkdir -p "$W" && cd "$W"

# 1. x264 (no assembler on this machine -> --disable-asm)
if [ ! -f x264out/lib/libx264.a ]; then
[ -d x264 ] || git clone --depth 1 https://code.videolan.org/videolan/x264.git
( cd x264
  ./configure --host=i686-w64-mingw32 --cross-prefix=i686-w64-mingw32- --enable-static --disable-cli --disable-asm \
    --disable-opencl --disable-avs --disable-swscale --disable-lavf --disable-ffms --disable-gpac --disable-lsmash \
    --prefix="$W/x264out"
  make -j8
  make install )
fi

# 2. ffmpeg 6.1.1 from the project's tarball, out-of-tree copy
[ -d ffsrc ] || { mkdir ffsrc && tar -xf "${FFMPEG_TARBALL:?set FFMPEG_TARBALL to the ffmpeg-6.1.1.tar.xz from ffmpeg.org}" -C ffsrc --strip-components=1; }
mkdir -p ffbuild && cd ffbuild
PKG_CONFIG_PATH="$W/x264out/lib/pkgconfig" PKG_CONFIG_LIBDIR="$W/x264out/lib/pkgconfig" \
../ffsrc/configure --prefix="$W/ffout" --arch=x86 --target-os=mingw32 --cross-prefix=i686-w64-mingw32- \
  --enable-gpl --enable-libx264 --pkg-config=pkg-config --pkg-config-flags=--static \
  --disable-shared --enable-static --disable-programs --disable-doc --disable-everything --disable-network \
  --disable-pthreads --enable-w32threads --disable-avdevice --disable-avfilter --disable-postproc --disable-x86asm --disable-debug \
  --enable-avformat --enable-avcodec --enable-swscale --enable-swresample \
  --enable-demuxer=asf,mov --enable-parser=vc1,mpegaudio,h264,aac \
  --enable-encoder=wmv2,wmv1,wmav2,wmav1,libx264,aac --enable-muxer=asf,mp4 \
  --enable-decoder=wmv1,wmv2,wmv3,vc1,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmav1,wmav2,wmapro,wmalossless,wmavoice,mp3,pcm_s16le \
  --enable-protocol=file --extra-cflags="-O2 -I$W/x264out/include" --extra-ldflags="-L$W/x264out/lib"
make -j8
make install
echo "DONE: $W/ffout (libx264 + aac + mp4)"
