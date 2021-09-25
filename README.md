# myh264
This is an incremental H.264 video codec based on x264 and FFmpeg that supports 
- Multi-frame RoI encoding 
- Macroblock-level error bound control

## Build (MacOS and Linux)
X264 and FFmpeg need to be rebuilt each time a modification is made in any of the two directories. For now, it is required that you have access to sudo on linux command line. To build, simply run
```
sudo ./build.sh
```
Warning: this will overwrite your locally installed version of FFmpeg

## Multi-frame RoI Encoding
For RoI encoding, make sure you leave the following content in operation_mode_file:
```
20,
```
In the myh264 folder, you will need a file named qp_matrix_file to store the qp values of all macroblocks. For 1280x720 videos, you'll need a 45x80 matrix indicating the macroblock-level qps for each frame. For example, the matrix below will lead the codec to encoder the entire left half of the frame in QP=1, and the entire right half of the frame in QP=40. You shall store the 45x80 matrices in qp_matrix_file line-by-line consecutively, i.e. line 1-45 will represent the QP matrix for frame 1, line 46-90 will represent the QP matrix for frame 2, etc.
```
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
...
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
```

You can now encode the video with
```
ffmpeg -y -i path_to_video_frames/%010d.png -start_number 0 -qp 10 -pix_fmt yuv420p video.mp4
```
You may use any value for the "-qp" field, which does not matter in RoI encoding.

## Uniform QP Encoding (MPEG)
If you'd like to encode the entire video with a uniform QP value of X, change the content of operation_mode_file to
```
0,X,
```
Then run with the usual ffmpeg encoding command. myh264 will ensure that in the encoded video, the QP value of every macroblock is strictly equal to X. Note that the bit-rate control module will be surpassed.

## Common Problems

### libavdevice not found
You might encounter the following error when running ffmpeg commands with myh264 installed:
```
ffmpeg: error while loading shared libraries: libavdevice.so.59: cannot open shared object file: No such file or directory
```
In this case, ffmpeg is not searching for libavdevice.so.59 in the directory. First, do
```
find /tank/your_cnet_id -name libavdevice.so.59
```
Add the returned directory to LD_LIBRARY_PATH. For example, I'll do
```
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/tank/qizheng/lib/
```
