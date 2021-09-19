cd x264
./configure --enable-shared --disable-asm
make
sudo make install
cd ../ffmpeg
./configure --enable-libx264 --enable-gpl --enable-shared
make
sudo make install
cd ..