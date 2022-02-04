cd x264
./configure --enable-shared --disable-asm --prefix=/tank/kuntai/
make
make install
export PATH=$PATH:/tank/kuntai/myh264/x264
cd ../ffmpeg-3.4.8
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/tank/kuntai/myh264/x264
./configure --enable-libx264 --enable-gpl --enable-shared --disable-x86asm --prefix=/tank/kuntai/ --extra-cflags="-I/tank/kuntai/myh264/x264" --extra-ldflags="-I/tank/kuntai/myh264/x264"
make
make install
cd ..
