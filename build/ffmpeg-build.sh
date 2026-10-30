#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/src"
./configure --prefix="$(dirname "$PWD")/out" --arch=x86 --target-os=mingw32 --cross-prefix=i686-w64-mingw32- \
  --disable-shared --enable-static --disable-programs --disable-doc --disable-everything --disable-network \
  --disable-pthreads --enable-w32threads --disable-avdevice --disable-avfilter --disable-postproc --disable-x86asm --disable-debug \
  --enable-avformat --enable-avcodec --enable-swscale --enable-swresample \
  --enable-demuxer=asf --enable-parser=vc1,mpegaudio \
  --enable-encoder=wmv2,wmv1,wmav2,wmav1 --enable-muxer=asf --enable-decoder=wmv1,wmv2,wmv3,vc1,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmav1,wmav2,wmapro,wmalossless,wmavoice,mp3,pcm_s16le \
  --enable-protocol=file --extra-cflags="-O2"
make -j2
make install
