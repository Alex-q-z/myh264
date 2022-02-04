directory=$(pwd)
cd x264
./configure --enable-shared --disable-asm --prefix=$directory
make
make install
export PATH=$PATH:$directory/x264
cd ../ffmpeg-3.4.8
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:$directory/x264
./configure --enable-libx264 --enable-gpl --enable-shared --disable-x86asm --prefix=$directory --extra-cflags="-I$directory/x264" --extra-ldflags="-I$directory/x264"
make
make install
cd ..
