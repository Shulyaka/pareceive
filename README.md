# pareceive [![Build Status](https://travis-ci.org/Shulyaka/pareceive.svg?branch=master)](https://travis-ci.org/Shulyaka/pareceive)
S/PDIF receiver with support of compressed formats

This tool is a PulseAudio application that captures the audio from S/PDIF (aka IEC 958) input and plays it back on an output device. When it encounteres a compressed (IEC61937) signal, it decodes it using ffmpeg.

**Warning:** Never tested on a real hardware yet.
