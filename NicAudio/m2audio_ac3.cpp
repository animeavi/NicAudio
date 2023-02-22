#pragma warning(disable:4996)
/*=============================================================================
        Module Name:
                        m2audio_ac3.cpp

        Abstract:
                        m2Audio AC-3 decoder plugin for AviSynth 2.5

        License:
            GNU General Public License (GPL)

        Revision History:
                        * Mar 29, 2004:  Created by Attila Afra
                        * Jul  5, 2004:  DAI (DVD Audio Index) input
                        * Jul 20, 2004:  Major bugfixes, improved error tolerance
                        * Sep 12, 2004:  DAI replaced with VFM
                        * Nov  6, 2004:  Bugfixes
                        * Feb 13, 2005:  Minor changes
                        * Feb 27, 2005:  Advanced dummy audio generation
                        * Feb 29, 2008:  !vfm >4GB support, Map matrix with all acmod allowed, dsurmod bugfix. Tebasuna

------------------------------------------------------------------------------
        Copyright © 2004-2005 Attila T. Afra. All rights reserved.
=============================================================================*/

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>
//#include "vfm.h"
#include "avisynth.h"
#include "AudioHeader.h"

/*-----------------------------------------------------------------------------
        Constants
-----------------------------------------------------------------------------*/

#define FRAME_HEADER_LEN        7                                       // length of a frame's header
#define BLOCKS_PER_FRAME        6                                       // number of blocks in a frame
#define SAMPLES_PER_BLOCK       256                                     // number of samples per channel in a block
#define SAMPLES_PER_FRAME       (BLOCKS_PER_FRAME * SAMPLES_PER_BLOCK)  // number of samples per channel in a frame

//#define MAX_DETECT_ERROR        1024                                    // max detection error (in bytes)
#define MAX_DETECT_ERROR        1048576                                   // max detection error (1 MB)
                                                                          // 13 sec 640 Kb/s, 21 for 192.
/*=============================================================================
        m2AudioAC3Source >> Implementation
=============================================================================*/

AVSValue __cdecl m2AudioAC3Source::Create(AVSValue args, void *, IScriptEnvironment *env)
{
        return new m2AudioAC3Source(args[0].AsString(), args[1].AsInt(0), args[2].AsInt(0), env);
}

/*-----------------------------------------------------------------------------
        m2AudioAC3Source >> Initialization
-----------------------------------------------------------------------------*/

// Constructor
m2AudioAC3Source::m2AudioAC3Source(const char *FileName, int Downmix, int DRC, IScriptEnvironment *env)
{
        int Channels;   // channel count
        int Sync;       // synch info
        int _DecFlags;  // temporary flags

        // Initialize pointers
        Stream = NULL;
        State  = 0;
        Frame  = 0;
        Buffer = 0;
        _drc = DRC;

        // Store filename
        StreamName = new char[strlen(FileName) + 1];
        strcpy(StreamName, FileName);

        // Open file
        if (!(Stream = fopen(StreamName, "rb")))
                env->ThrowError("m2AudioAC3Source: unable to open file \"%s\"", StreamName);

        _fseeki64(Stream, 0, SEEK_END);          // instead vfm for >4 GB size
        StreamLength = _ftelli64(Stream);
        _fseeki64(Stream, 0, SEEK_SET);

        // Empty information structure
        memset(&Info, 0, sizeof(VideoInfo));

        // Check if the stream is empty
        if (!StreamLength)
        {
                // Dummy audio
                Info.sample_type              = SAMPLE_INT16;
                Info.nchannels                = Downmix ? Downmix : 2;
                Info.audio_samples_per_second = 44100;
                Info.num_audio_samples        = 0;

                SampleCount                   = 0;

                return;
        }

        // Find first frame
        FrameLength = MAX_DETECT_ERROR;
        Sync        = Synchronize(FrameLength, Flags, Samplerate, Bitrate);
        if (Sync <= 0)
                env->ThrowError("m2AudioAC3Source: \"%s\" without a valid AC-3 header in first 1MB", StreamName);

        StreamOffset = (Sync - 1) % FrameLength;
        _fseeki64(Stream, StreamOffset, SEEK_SET);

        // Set sample type
        Info.sample_type = SAMPLE_FLOAT;

        // Get channel count
        DecFlags = Flags;                          // flags = acmod + 8*dsurmod + 16*lfeon
        LFE      = ((Flags & A52_LFE) == A52_LFE); // WAS Flags && A52_LFE !!!!!
        Channels = Flags & A52_CHANNEL_MASK;

        switch (Channels)
        {
        case A52_MONO:
                ChannelCount = 1;
                break;
        case A52_STEREO:
        case A52_DOLBY:
        case A52_CHANNEL:
                ChannelCount = 2;
                break;
        case A52_3F:
        case A52_2F1R:
                ChannelCount = 3;
                break;
        case A52_3F1R:
        case A52_2F2R:
                ChannelCount = 4;
                break;
        case A52_3F2R:
                ChannelCount = 5;
                break;
        default:
                env->ThrowError("m2AudioAC3Source: unsupported channel configuration in file \"%s\"", StreamName);
        }
        if (LFE) ChannelCount++;


        if (Downmix)                               // Not recommended use this internal downmix here
        {                                          // but for compatibility ...
                // Downmix
                switch (Downmix)
                {
                // 1 channel
                case 1:
                        _DecFlags = A52_MONO;
                        break;

                // 2 channels (Dolby Surround)
                case 2:
                        _DecFlags = A52_DOLBY;
                        break;

                // 4 channels
                case 4:
                        _DecFlags = A52_2F2R;
                        break;

                // 6 channels
                case 6:
                        _DecFlags = A52_3F2R;
                        if (LFE)
                                _DecFlags |= A52_LFE;
                        break;

                default:
                        env->ThrowError("m2AudioAC3Source: invalid number of channels requested");
                }

                if (Downmix < ChannelCount)
                {
                        ChannelCount = Downmix;
                        DecFlags     = _DecFlags;
                }
        }
        Info.nchannels = ChannelCount;

        // Remapping Matrix
        for (Channels=0; Channels < ChannelCount; Channels++) map[Channels] = Channels;

        switch (DecFlags & 23) {                                    //acmod + 16*LFE
//      case 1:      // 0                1 channel                              ok
//      case 0:      // 0, 1             2 chan: Channel1, Channel2             ok
//      case 2:      // 0, 1             2 chan: left, right                    ok
//      case 4:      // 0, 1, 2          2/1 chan: FLeft, FRight, RCenter       ok
//      case 6:      // 0, 1, 2, 3       2/2 chan: FLeft, FRight, RLeft, RRight ok
        case 3:      // 0, 2, 1          3/0 chan: FLeft, FRight, FCenter
        case 5:      // 0, 2, 1, 3       3/1 chan: FLeft, FRight, FCenter, RCenter
        case 7:      // 0, 2, 1, 3, 4    3/2 chan: FLeft, FRight, FCenter, RLeft, RRight
               map[1] = 2;
               map[2] = 1;
               break;
        case 17:     // 1, 0             2 chan: 1 mono + LFE
               map[0] = 1;
               map[1] = 0;
               break;
        case 16:     // 2, 0, 1          2/0.1 chan: left, right + LFE
        case 18:     // 2, 0, 1          3 chan: Channel1, Channel2 + LFE
        case 20:     // 2, 0, 1, 3       2/1.1 chan: FLeft, FRight + LFE, RCenter
        case 22:     // 2, 0, 1, 3, 4    2/2.1 chan: FLeft, FRight + LFE, RLeft, RRight
               map[0] = 2;
               map[1] = 0;
               map[2] = 1;
               break;
        case 19:     // 3, 0, 2, 1       3/0.1 chan: FLeft, FRight, FCenter + LFE
        case 21:     // 3, 0, 2, 1, 4    3/1.1 chan: FLeft, FRight, FCenter + LFE, RCenter
        case 23:     // 3, 0, 2, 1, 4, 5 3/2.1 chan: FLeft, FRight, FCenter + LFE, RLeft, RRight
               map[0] = 3;
               map[1] = 0;
               map[3] = 1;
        }

        // Enable automatic level adjustment
        DecFlags |= A52_ADJUST_LEVEL;
        Level     = 1;

        // Get sampling rate
        Info.audio_samples_per_second = Samplerate;

        // Get number of frames
        FrameCount = int((StreamLength - StreamOffset) / FrameLength);

        // Nick: tebasuna51 suggestion
//      int FrameCount2 = int( ( 147 * (StreamLength - StreamOffset) / (640 * (Bitrate/1000)) ) );
//      int FrameCount3 = int(((147.0 * double(StreamLength - StreamOffset)) / ( 640.0 * double(Bitrate/1000)) )+0.5);
        if (Samplerate == 44100)
                FrameCount = int(((147.0 * double(StreamLength - StreamOffset)) / ( 640.0 * double(Bitrate/1000)) )+0.5);

        // Get number of samples
        SampleCount            = __int64(FrameCount) * SAMPLES_PER_FRAME;
        Info.num_audio_samples = SampleCount;

        // Initialize buffers
        Frame       = new unsigned char[FrameLength*2];         // Nick: *2 to be on the safe side ;)
        FrameIndex  = -2;

        BufferSize  = Info.nchannels * SAMPLES_PER_FRAME;
        Buffer      = new float[BufferSize];
        BufferStart = BufferEnd = -1;

        // Set decoder acceleration
        Accel = 0;
}

// Destructor
m2AudioAC3Source::~m2AudioAC3Source()
{
        // Close file
        if (Stream)
                fclose(Stream);

        // Free memory
        if (State)
                a52_free(State);

        if (Frame)
                delete[] Frame;

        if (Buffer)
                delete[] Buffer;
}

/*-----------------------------------------------------------------------------
        m2AudioAC3Source >> IClip interface
-----------------------------------------------------------------------------*/

// Return VideoInfo
const VideoInfo& __stdcall m2AudioAC3Source::GetVideoInfo()
{
        return Info;
}

// Return video frame
PVideoFrame __stdcall m2AudioAC3Source::GetFrame(int n, IScriptEnvironment *env)
{
        return NULL;
}

// Return video field parity
bool __stdcall m2AudioAC3Source::GetParity(int n)
{
        return false;
}

// Return audio samples
void __stdcall m2AudioAC3Source::GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env)
{
        __int64   i;                    // cycle counter
        int      _FrameIndex;           // index of the previous frame
        float   *Input, *Output;        // buffer pointers
        __int64  Left;                  // number of samples to be copied from the buffer

        // Check if the requested samples are not out of range
        if (!SampleCount || (start < 0) || (count < 1))
                return;

        if (start + count > SampleCount)
                count = SampleCount - start;

        // Check if we must read a frame
        if (start < BufferStart || start > BufferEnd)
        {
                // Calculate the index and the sample range of the required frame
                _FrameIndex = FrameIndex;
                FrameIndex  = int(start / SAMPLES_PER_FRAME);
                BufferStart = __int64(FrameIndex) * SAMPLES_PER_FRAME;
                BufferEnd   = BufferStart + SAMPLES_PER_FRAME;

                // Seek to the frame, if required
                if (FrameIndex != _FrameIndex + 1)
                {
                        // Reset decoder
                        if (State)
                                a52_free(State);
                        State = a52_init(Accel);

                        if (FrameIndex == 0)
                                _fseeki64(Stream, StreamOffset, SEEK_SET);
                        else
                        {
                                _fseeki64(Stream, StreamOffset + __int64(FrameIndex - 1) * __int64(FrameLength), SEEK_SET);

                                // Read previous frame to initialize the desired frame
                                if (!ReadFrame())
                                        env->ThrowError("m2AudioAC3Source: error in file \"%s\"", StreamName);
                        }
                }

                // Read the frame
                if (!ReadFrame())
                        env->ThrowError("m2AudioAC3Source: error in file \"%s\"", StreamName);
        }

        // Start decoding
        Input  = Buffer + int(start - BufferStart) * ChannelCount;
        Output = (float *)buf;
        Left   = SAMPLES_PER_FRAME - (start - BufferStart);

        while (count)
        {
                // Copy samples
                if (count < Left) Left = count;

                count -= Left;

                for (i = 0; i < Left * ChannelCount; i++)
                        *Output++ = *Input++;

                // Check if we must read a frame
                if (count)
                {
                        // Read the frame
                        FrameIndex++;
                        if (!ReadFrame())
                                env->ThrowError("m2AudioAC3Source: error in file \"%s\"", StreamName);

                        // Reset input buffer pointer
                        Input = Buffer;
                        Left  = SAMPLES_PER_FRAME;
                }
        }

        // Calculate the sample range of the current frame
        BufferStart = __int64(FrameIndex) * SAMPLES_PER_FRAME;
        BufferEnd   = BufferStart + SAMPLES_PER_FRAME;
}

// Set cache hints
void __stdcall m2AudioAC3Source::SetCacheHints(int cachehints, int frame_range)
{
}

/*-----------------------------------------------------------------------------
        m2AudioAC3Source >> Operations
-----------------------------------------------------------------------------*/

// Synchronize with the audio stream
int m2AudioAC3Source::Synchronize(int& Length, int& Flags, int& Samplerate, int& Bitrate)
{
        int Delta = 0;                                                          // number of bytes out of synch
        int _Length;                                                            // backup of Length
        unsigned char Buffer[FRAME_HEADER_LEN];         // buffer for a frameheader

        // Backup Length
        _Length = Length;

        // Try synchronization
        while (Delta < _Length - 1)
        {
                // Read header
                if (fread(Buffer, 1, FRAME_HEADER_LEN, Stream) != FRAME_HEADER_LEN)
                        return -1;

                // Get information
                Length = a52_syncinfo(Buffer, &Flags, &Samplerate, &Bitrate);

                if (Length)
                {
                        // Success
                        _fseeki64(Stream, -FRAME_HEADER_LEN, SEEK_CUR);
                        return (Delta + 1);
                }
                else
                {
                        // Try again
                        _fseeki64(Stream, 1 - FRAME_HEADER_LEN, SEEK_CUR);
                        Delta++;
                }
        }

        // Failure
        Length = _Length;
        return 0;
}

// Reads the next frame from the stream and decodes it into the framebuffer
// Nick: New Function from tebasuna51

bool m2AudioAC3Source::ReadFrame()
{
        int Sync;                                                       // synch info
        int Length;                                                     // length of the current frame
        int _Flags, _Samplerate, _Bitrate;      // bit stream information

        // Read next frame from stream
        if (fread(Frame, 1, FrameLength, Stream) != FrameLength)
        {
                // Error at the end of the stream, mute frame
                EmptyFrame();
                return true;
        }

        // Get frame information
        Length = a52_syncinfo(Frame, &_Flags, &_Samplerate, &_Bitrate);

        // Check if we need synchronization
        if (!Length)
        {
                // Seek back in the stream
                _fseeki64(Stream, 1 - FrameLength, SEEK_CUR);

                // Try synchronization
                Length = FrameLength;
                Sync   = Synchronize(Length, _Flags, _Samplerate, _Bitrate);

                // Serious damage in the stream, mute frame
                if (!Sync || Sync < 0)
                {
                        // Mute frame
                        EmptyFrame();
                        return true;
                }

                // Check if the frame has proper length and BSI
//              if ( (Length != FrameLength && Samplerate != 44100) || ( _Flags != Flags ) || (_Samplerate != Samplerate))
                if ( (Length != FrameLength && Samplerate != 44100) || ( (_Flags & 23)  != (Flags & 23)) || (_Samplerate != Samplerate))
                {
                        /*
                        // Fatal error, unable to continue decoding
                        return false;
                        */
                        EmptyFrame();           // Could just be a broken frame. Blank out and continue :D
                        return true;
                }
                else
                // Handle 44100khz's weird two different FrameLengths
                if ( (Length != FrameLength && Samplerate == 44100) )
                {
                        // Just make sure it's not a ludicrous difference (difference should only be 1 )
                        if ( abs(Length-FrameLength) > 2 )
                                return false;

                        FrameLength = Length;
                }

                // Read frame
                if (fread(Frame, 1, FrameLength, Stream) != FrameLength)
                {
                        // Error at the end of the stream, mute frame
                        EmptyFrame();
                        return true;
                }
        }
        else
        {
                // Check if the frame has proper length and BSI
//@             if ( (Length != FrameLength && Samplerate != 44100) || ( _Flags != Flags ) || (_Samplerate != Samplerate))
                if ( (Length != FrameLength && Samplerate != 44100) || ((_Flags & 23) != (Flags & 23)) || (_Samplerate != Samplerate))
                {
                        /*
                        // Fatal error, unable to continue decoding
                        return false;
                        */
                        EmptyFrame();           // Could just be a broken frame. Blank out and continue :D
                        return true;
                }
                else
                // Handle 44100khz's weird two different FrameLengths
                if ( (Length != FrameLength && Samplerate == 44100) )
                {
                        // Just make sure it's not a ludicrous difference (difference should only be 1 )
                        if ( abs(Length-FrameLength) > 2 )
                        {
                                return false;
                        }

                        // Re-do all this with new FrameLength
                        _fseeki64(Stream, -FrameLength, SEEK_CUR);
                        FrameLength = Length;
                        return ReadFrame();             // Recursion!
                }
        }

        // Decode frame
        if (!DecodeFrame())
                EmptyFrame();

        return true;
}

// Decode frame from the raw buffer
bool m2AudioAC3Source::DecodeFrame()
{
        int    blk, i, j;       // cycle counters
        float *Input, *Output;  // buffer pointers

        // Start decoding the frame
        if (a52_frame(State, Frame, &DecFlags, &Level, 0))
                return false;

        // Dynamic range compression !! dimzon !!

        if(0==_drc)
                a52_dynrng(State, 0, 0);

        // The decoded samples will be in the following sequencial order:
        //   LFE, front left, front center, front right, rear left, rear right
        // If one of the channels is not present, it is skipped and the following
        // channels are shifted accordingly. We have to put the channels in
        // the proper interleaved order, based on the requested channel config.
        Output = Buffer;  // Decode each block
        for (blk = 0; blk < BLOCKS_PER_FRAME; blk++) {

            if (a52_block(State)) return false; // Decode block
            Input = a52_samples(State);         // Copy channel

            for (j = 0; j < ChannelCount; j++) {
                for (i = 0; i < SAMPLES_PER_BLOCK; i++) {
                      Output[i * ChannelCount + map[j]] = *Input++;     // mapping matrix
//                        Output[i * ChannelCount + map[j]] = Input[0];     // mapping matrix
//                        Input += 1;

                }
            }
            Output += SAMPLES_PER_BLOCK * ChannelCount;                   // Next block
        }
        return true;
}

// Empty the framebuffer
void m2AudioAC3Source::EmptyFrame()
{
        for (int i = 0; i < SAMPLES_PER_FRAME * ChannelCount; i++)
                Buffer[i] = 0;
}

/*-----------------------------------------------------------------------------
        AviSynth plugin initialization
-----------------------------------------------------------------------------*/

/*
extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2(IScriptEnvironment *env)
{
        env->AddFunction("NicAC3Source", "s[channels]i[drc]i", m2AudioAC3Source::Create, 0);

        return "Audio Decoder Plugin";
}
*/
