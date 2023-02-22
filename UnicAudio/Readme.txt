uNicAudio.dll v2.0.6 - AviSynth Audio Plugins for MPEG Audio/AC3/DTS/LPCM and other uncompressed formats
                       Based in NicAudio.dll v2.0.6 with a unique function call for all the supported formats
                       Same libs used, AudioSource.cpp replace mpasource.cpp and m2audio_*.cpp in NicAudio.

AviSynth USAGE
--------------

Syntax:
   UnicAudioSource("FileName.ext", int "Param1", int "Channels", int "SampleBits")

Where the big difference is the parameter meaning with different sources:
   AC3/DTS
      "Param1"     Old DRC, if 1 Apply Dynamic Range Compression algorytm. Else don't apply
      "Channels"   Maximum number of channels to output (Downmix). Optional.

   MPA/MP2/MP3
      "Param1"     Old Normalize, if 1 Normalize, if 2 write auxiliary d2a file, 3 both. Else nothing.
                   Normalize means apply the max gain without clip signal.

   RAW/PCM
      "Param1"     Old SampleRate. Necessary for lpcm and raw files.
      "Channels"   Necessary for lpcm and raw files. Max 8 channels.
      "SampleBits" Necessary for lpcm and raw files. Valid values 8/16/24/32 (also 33 (32 float) for raw)
                   (lpcm also accept 20 and -8/-16/-24/-32. Negative values are for BluRay lpcm (big-endian))

   WAV (and other uncompressed formats)
      "Param1"     IgnoreLength 1 force ignore the data size read in the header also in 64 bits formats.
                                2 ignore the data size 32 bits read in the header if > 2GB. (default)
                                4 ignore the data size 32 bits read in the header if > 4GB.

Supported files (all can be > 4GB):
   ac3          also in wav container (TODO: support also eac3 files or Dolby Digital Plus)
   dts          also dtswav and dtshd (only core decoded) supported
   mpa          mpeg files: mp1, mp2 and mp3 (also in wav container)
   lpcm         from DVD Audio, from BluRay with -SampleBits
   raw          uncompressed audio without header but with standard MS channel order and endian.
   wav          and other uncompressed formats: WAV, WAVE_FORMAT_EXTENSIBLE, W64, BWF, RF64, AU, AIFF and CAF

Examples:

LoadPlugin("uNicAudio.dll")
UnicAudioSource("c:\File.ac3", 1, 2)           # Apply DRC and downmix to stereo

UnicAudioSource("c:\File.dts")                 # preserve full channels and quality

UnicAudioSource("c:\File.mp3", 3)              # normalize and write the .d2a file for quick acces next time

UnicAudioSource("c:\File.lpcm", 48000, 6, -24) # we need the 3 param. to open raw or pcm files (.pcm or .lpcm)

UnicAudioSource("c:\File.w64", 1)              # the extension isn't mandatory, the header are checked


CHANGE LOG
----------
See the NicAudio changelog for same version.
2008/10/30 Tebasuna first version 2.0.2.

First version from the NicAudio.dll 2.0.2 version. Differences with NicAudio:

- A unique function call instead 5 different.
- The bool param in NicMPG123Source now is int. The last parameter order in RAW/LPCM was changed.
- Now we can detect mp3, ac3 (VirtualDub) and dts (dtswav) in wav container and use the appropriate decoder.

Warning: WAV is a container and other compressed formats aren't supported. If you have this error:
UnicSource: Unssuported header format xxxx
try opening the file with the standard WavSource()

CREDITS
-------
All credit should go to the excellent creator of these original filters
used in FilmShrink.sf.net - Attila T. Afra

Filters compiled and gathered together by Nic. All under the GPL.

Other patches by Dimzon, IanB, and Tebasuna.
