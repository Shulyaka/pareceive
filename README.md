# pareceive
S/PDIF receiver with support of compressed formats

This tool is a PulseAudio application that captures the audio from S/PDIF (aka IEC 958) input and playbacks it back on an output device. When it encounteres a compressed (IEC61937) signal, it decodes it using ffmpeg.

Current status: Not tested on a real hardware.
There might be issues with detection of a sample rate.
