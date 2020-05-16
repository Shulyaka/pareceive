# pareceive [![Build Status](https://travis-ci.org/Shulyaka/pareceive.svg?branch=master)](https://travis-ci.org/Shulyaka/pareceive)
S/PDIF receiver with support of compressed formats

This tool is a PulseAudio application that captures the audio from S/PDIF (aka IEC 958) input and plays it back on an output device. When it encounteres a compressed (IEC61937) signal, it decodes it using libav.

Latest stable version of PulseAudio is recommended. Older PulseAudio versions may have issues with latency.

Tested on Raspberry Pi with HiFiBerry Digi+ I/O board as SPDIF input device and ST Lab M-330 USB soundard as 7.1 DAC output device
