

# myh264
Este es un códec de video H.264 incremental basado en x264 y FFmpeg que admite 
- Codificación RoI multiframe 
- Control de límite de error a nivel de macrobloque

## Compilación (MacOS y Linux)
X264 y FFmpeg deben recompilarse cada vez que se realice una modificación en cualquiera de los dos directorios. Por ahora, es necesario que tengas acceso a sudo en la línea de comandos de Linux. Para compilar, simplemente ejecuta
```
sudo ./build.sh
```
Advertencia: esto sobrescribirá la versión local de FFmpeg que tienes instalada

## Codificación RoI multiframe
Para la codificación RoI, asegúrate de dejar el siguiente contenido en operation_mode_file:
```
20,
```
En la carpeta myh264, necesitarás un archivo llamado qp_matrix_file para almacenar los valores de qp de todos los macrobloques. Para videos de 1280x720, necesitarás una matriz de 45x80 que indique los qps a nivel de macrobloque para cada frame. Por ejemplo, la matriz a continuación hará que el códec codifique la mitad izquierda completa del frame con QP=1, y la mitad derecha completa del frame con QP=40. Deberás almacenar las matrices de 45x80 en qp_matrix_file línea por línea de forma consecutiva, es decir, las líneas 1-45 representarán la matriz de QP para el frame 1, las líneas 46-90 representarán la matriz de QP para el frame 2, etc.
```
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
...
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 
```

Ahora puedes codificar el video con
```
ffmpeg -y -i path_to_video_frames/%010d.png -start_number 0 -qp 10 -pix_fmt yuv420p video.mp4
```
Puedes usar cualquier valor para el campo "-qp", ya que no importa en la codificación RoI.

## Codificación de QP uniforme (MPEG)
Si deseas codificar todo el video con un valor de QP uniforme X, cambia el contenido de operation_mode_file a
```
0,X,
```
Luego ejecuta el comando habitual de codificación de ffmpeg. myh264 se asegurará de que en el video codificado, el valor de QP de cada macrobloque sea estrictamente igual a X. Ten en cuenta que el módulo de control de bitrate se ignorará.

## Problemas comunes

### libavdevice no encontrado
Es posible que te encuentres con el siguiente error al ejecutar comandos de ffmpeg con myh264 instalado:
```
ffmpeg: error while loading shared libraries: libavdevice.so.59: cannot open shared object file: No such file or directory
```
En este caso, ffmpeg no está buscando libavdevice.so.59 en el directorio. Primero, ejecuta
```
find /tank/your_cnet_id -name libavdevice.so.59
```
Agrega el directorio devuelto a LD_LIBRARY_PATH. Por ejemplo, yo haría
```
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/tank/qizheng/lib/
```
