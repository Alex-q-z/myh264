#!/bin/bash
# Get an updated config.sub and config.guess
cp $BUILD_PREFIX/share/gnuconfig/config.* .
set -xe
mkdir -vp ${PREFIX}/bin

# Set the assembler to `nasm`
if [[ ${target_platform} == "linux-64" || ${target_platform} == osx-64 ]]; then
    export AS="${BUILD_PREFIX}/bin/nasm"
fi

if [[ "${target_platform}" == *-aarch64 || "${target_platform}" == *-arm64 ]]; then
    unset AS
fi

cd /home/myh264/x264

chmod +x configure
# Using --system-libx264 links the executable to the shared lib not the static library
./configure \
        --host=$HOST \
        --enable-pic \
        --enable-shared \
        --system-libx264 \
        --prefix=${PREFIX}

make -j${CPU_COUNT}
make install -j${CPU_COUNT}