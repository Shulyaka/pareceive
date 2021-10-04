[![Codacy Badge](https://api.codacy.com/project/badge/Grade/41e42cb712ba443eacf8b355cb6a32ec)](https://app.codacy.com/gh/Shulyaka/pareceive?utm_source=github.com&utm_medium=referral&utm_content=Shulyaka/pareceive&utm_campaign=Badge_Grade_Settings)
# pareceive [![Build Status](https://app.travis-ci.com/Shulyaka/pareceive.svg?branch=master)](https://app.travis-ci.com/Shulyaka/pareceive) [![Total alerts](https://img.shields.io/lgtm/alerts/g/Shulyaka/pareceive.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/Shulyaka/pareceive/alerts/) [![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/Shulyaka/pareceive.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/Shulyaka/pareceive/context:cpp)
S/PDIF receiver with support of compressed formats

This tool is a PulseAudio application that captures the audio from S/PDIF (aka IEC 958) input and plays it back on an output device. When it encounteres a compressed (IEC61937) signal, it decodes it using libav.

Latest stable version of PulseAudio is recommended. Older PulseAudio versions may have issues with latency.

Tested on Raspberry Pi with HiFiBerry Digi+ I/O board as SPDIF input device and ST Lab M-330 USB soundard as 7.1 DAC output device

If you have an issue with input device over pulseaudio, you can also try piping audio from `arecord` via stdin:
```
arecord -D hw:CARD=sndrpihifiberry,DEV=0 -q -C -f s16_le -c 2 -t raw --disable-channels --disable-format --disable-resample --disable-softvol | pareceive -
```
If this is the case, you may also want to tell pulseaudio to ignore the card via udev rules.
