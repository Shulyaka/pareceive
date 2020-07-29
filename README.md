# pareceive [![Build Status](https://travis-ci.org/Shulyaka/pareceive.svg?branch=master)](https://travis-ci.org/Shulyaka/pareceive)
S/PDIF receiver with support of compressed formats

This tool is a PulseAudio application that captures the audio from S/PDIF (aka IEC 958) input and plays it back on an output device. When it encounteres a compressed (IEC61937) signal, it decodes it using libav.

Latest stable version of PulseAudio is recommended. Older PulseAudio versions may have issues with latency.

Tested on Raspberry Pi with HiFiBerry Digi+ I/O board as SPDIF input device and ST Lab M-330 USB soundard as 7.1 DAC output device

If you have an issue with input device over pulseaudio, you can also try piping audio from `arecord` via stdin:
```
arecord -D hw:CARD=sndrpihifiberry,DEV=0 -q -C -f s16_le -c 2 -t raw --disable-channels --disable-format --disable-resample --disable-softvol | pareceive -
```
If this is the case, you may also want to tell pulseaudio to ignore the card via udev rules.
