#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/x264-debug issues/970_worklog/step2
(
    cd build/x264-debug
    ../../x264/configure --enable-debug --disable-asm --disable-opencl \
        --disable-lavf --disable-ffms --disable-avs --disable-swscale \
        --bit-depth=8 --chroma-format=420 \
        --extra-cflags="-O0 -g3 -fno-omit-frame-pointer -DX264_FLOW_TRACE"
    make -j2
) > issues/970_worklog/step2/2.1.1_instrumented_build.log 2>&1
