#!/bin/bash

set -x

cd x264
make clean
./configure --enable-shared --disable-asm --enable-pic 
make -j8
make install
cd ..

git clone https://github.com/FFmpeg/FFmpeg.git ffmpeg 
cd ffmpeg 
git checkout n4.3
./configure --enable-libx264 --enable-gpl --enable-shared  --disable-doc 
make -j8 && make install
cd ..

mkdir -p libs
rm -rf libs/*
LIB_PATH=`realpath libs`

# copy libs to single dir
find "x264/" -name "*.so.*" -exec cp {} libs/ \;
find "ffmpeg/" -name "*.so.*" -exec cp {} libs/ \;

LD_LIBRARY_PATH=$LIB_PATH ./ffmpeg/ffmpeg -y -loglevel debug -pix_fmt yuv420p -i assets/frame-%d.png -start_number 0 video.mp4