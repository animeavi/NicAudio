#pragma once

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>

#include "avisynth.h"

/*-----------------------------------------------------------------------------
        m2AudioAC3Source: Input filter class
-----------------------------------------------------------------------------*/
extern "C"
{
#include "liba52\a52.h"
#include "liba52\a52_internal.h"
//#include "mm_accel.h"
}

class m2AudioAC3Source : public IClip
{
public:
        static AVSValue __cdecl Create(AVSValue args, void *, IScriptEnvironment *env);

        // Initialization
        m2AudioAC3Source(const char *FileName, int Downmix, int DRC, IScriptEnvironment *env);
        ~m2AudioAC3Source();

        // IClip interface
        const VideoInfo& __stdcall GetVideoInfo();
        PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *env);
        bool __stdcall GetParity(int n);
        void __stdcall GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env);
        void __stdcall SetCacheHints(int cachehints, int frame_range);

private:
        // General information
        VideoInfo Info;                         // required by AviSynth

        // Stream
        FILE    *Stream;                        // stream handle
        char    *StreamName;                    // name of the stream
        __int64  StreamOffset;                  // offset to start of the audio data
        __int64  StreamLength;                  // total length of the stream

        // Audio information
        int     ChannelCount;                   // number of channels
        __int64 SampleCount;                    // number of samples per channel in the stream
        int     FrameCount;                     // number of entire frames in the stream
        int     Flags, Samplerate, Bitrate;     // bit stream information
        bool    LFE;                            // is the LFE channel present?
        int     DecFlags;                       // flags used for decoding
        float   Level;                          // level of output samples
        int     Accel;                          // decoder acceleration
        int     _drc;                           // DRC

        // Buffers
        a52_state_t   *State;                   // decoder state structure
        unsigned char *Frame;                   // raw buffer
        int FrameIndex, FrameLength;            // index and length of the current frame

        float   *Buffer;                        // samples from the current frame in interleaved order (framebuffer)
        int      BufferSize;                    // size of the framebuffer
        __int64  BufferStart, BufferEnd;        // sample range of the framebuffer

        int      map[8];                        // remap channels

        // Operations
        int Synchronize(int& Length, int& Flags, int& Samplerate, int& Bitrate);
        bool ReadFrame();
        bool DecodeFrame();
        void EmptyFrame();
};

/*-----------------------------------------------------------------------------
        m2AudioDTSSource: Input filter class
-----------------------------------------------------------------------------*/
extern "C"
{
#include "libdts\dts.h"
#include "libdts\dts_internal.h"
//#include "mm_accel.h"
}

class m2AudioDTSSource : public IClip
{
public:
        static AVSValue __cdecl Create(AVSValue args, void *, IScriptEnvironment *env);

        // Initialization
        m2AudioDTSSource(const char *FileName, int Downmix, int DRC, IScriptEnvironment *env);
        ~m2AudioDTSSource();

        // IClip interface
        const VideoInfo& __stdcall GetVideoInfo();
        PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *env);
        bool __stdcall GetParity(int n);
        void __stdcall GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env);
        void __stdcall SetCacheHints(int cachehints, int frame_range);

private:
        // General information
        VideoInfo Info;                         // required by AviSynth

        // Stream
        FILE    *Stream;                        // stream handle
        char    *StreamName;                    // name of the stream
        __int64  StreamOffset;                  // offset to start of the audio data
        __int64  StreamLength;                  // total length of the stream

        // Audio information
        int     ChannelCount;                   // number of channels
        __int64 SampleCount;                    // number of samples per channel in the stream
        int     FrameCount;                     // number of entire frames in the stream
        int     BlocksPerFrame;                 // number of blocks in a frame
        int     SamplesPerFrame;                // number of samples per channel in a frame
        int     Flags, Samplerate, Bitrate;     // bit stream information
        bool    LFE;                            // is the LFE channel present?
        int     DecFlags;                       // flags used for decoding
        float   Level;                          // level of output samples
        int     Accel;                          // decoder acceleration
        int     _drc;                           // DRC

        // Buffers
        dts_state_t   *State;                   // decoder state structure
        unsigned char *Frame;                   // raw buffer
        int FrameIndex, FrameLength;            // index and length of the current frame
        int FrameExt;                           // Extension of frame (padd, HR, ...)

        float   *Buffer;                        // samples from the current frame in interleaved order (framebuffer)
        int      BufferSize;                    // size of the framebuffer
        __int64  BufferStart, BufferEnd;        // sample range of the framebuffer

        int      map[8];                        // remap channels

        // Operations
        int  Synchronize(int& Length, int& Flags, int& Samplerate, int& Bitrate);
        int  ReadFrame();
        bool DecodeFrame();
        void EmptyFrame();
};

/*-----------------------------------------------------------------------------
        m2AudioLPCMSource: Input filter class
-----------------------------------------------------------------------------*/

class m2AudioLPCMSource : public IClip
{
public:
        static AVSValue __cdecl Create(AVSValue args, void *, IScriptEnvironment *env);

        // Initialization
        m2AudioLPCMSource(const char *FileName, int SampleRate, int SampleBits, int Channels, IScriptEnvironment *env);
        ~m2AudioLPCMSource();

        // IClip interface
        const VideoInfo& __stdcall GetVideoInfo();
        PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *env);
        bool __stdcall GetParity(int n);
        void __stdcall GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env);
        void __stdcall SetCacheHints(int cachehints, int frame_range);

private:
        // General information
        VideoInfo Info;                                 // required by AviSynth

        // Stream
        FILE  *Stream;                                  // stream handle
        char   *StreamName;                             // name of the stream
        __int64 StreamLength;                           // total length of the stream

        // Audio information
        int     Quant, InQuant;                         // sample quantization (number of bits per sample)
        int     ChannelCount;                           // number of channels
        __int64 SampleCount;                            // number of samples per channel in the stream
        int     SamplesPerFrame;                        // number of samples per channel in a frame
        int     InBlock;                                // ChannelCount * InQuant / 8 (input bytes per sample)
        int     OutBlock;                               // ChannelCount * Quant / 8 (output bytes per sample)

        // Buffers
        unsigned char *Frame;                           // raw buffer
        int            FrameIndex, FrameLength;         // index and length of the current frame

        unsigned char *Buffer;                          // processed samples from the current frame (pointer to the real framebuffer)
        __int64        BufferStart, BufferEnd;          // sample range of the framebuffer

        bool           bIsBluRay;                       // other lpcm order
        int            map[8];                          // remap channels

        // Operations
        bool ReadFrame();
};

/*-----------------------------------------------------------------------------
        m2RaWavSource: Input filter class
-----------------------------------------------------------------------------*/

class m2RaWavSource : public IClip
{
public:
        static AVSValue __cdecl Create(AVSValue args, void *, IScriptEnvironment *env);

        // Initialization
        m2RaWavSource(const char *FileName, int SampleRate, int SampleBits, int Channels, IScriptEnvironment *env);
        ~m2RaWavSource();

        // IClip interface
        const VideoInfo& __stdcall GetVideoInfo();
        PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *env);
        bool __stdcall GetParity(int n);
        void __stdcall GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env);
        void __stdcall SetCacheHints(int cachehints, int frame_range);

private:
        // General information
        VideoInfo Info;                                // required by AviSynth

        // Stream
        FILE    *Stream;                               //@ stream handle
        char    *StreamName;                           // name of the stream
        __int64 StreamLength;                          // total length of the stream

        // Audio information
        __int64 BlockAlign;                            // number of channels x number of bytes per sample
        __int64 SampleCount;                           // number of samples per channel in the stream
        __int64 HeaderSize;                            //@ number of bytes of audio header
};
