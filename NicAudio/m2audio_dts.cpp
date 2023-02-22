#pragma warning(disable:4996)
#pragma warning(disable:4244)
/*=============================================================================
        Module Name:
                        m2audio_dts.cpp

        Abstract:
                        m2Audio DTS decoder plugin for AviSynth 2.5

        License:
            GNU General Public License (GPL)

        Revision History:
                        * Apr 16, 2004:  Created by Attila Afra
                        * Jul  9, 2004:  DAI (DVD Audio Index) input
                        * Jul 20, 2004:  Major bugfixes, improved error tolerance
                        * Sep 12, 2004:  DAI replaced with VFM
                        * Nov  6, 2004:  Bugfixes
                        * Feb 13, 2005:  Minor changes
                        * Feb 27, 2005:  Advanced dummy audio generation
                        * May 15, 2008:  VFM deprecated, out-channels 1..6, core decode

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

#define SAMPLES_PER_BLOCK       256                     // number of samples per channel in a block

#define MAX_DETECT_ERROR        2048                    // max detection error (in bytes), max padd also.

#define FRAME_HEADER_LEN        14                      // length of a frame's header



/*=============================================================================
        m2AudioDTSSource >> Implementation
=============================================================================*/

AVSValue __cdecl m2AudioDTSSource::Create(AVSValue args, void *, IScriptEnvironment *env)
{
        return new m2AudioDTSSource(args[0].AsString(), args[1].AsInt(0), args[2].AsInt(0), env);
}

/*-----------------------------------------------------------------------------
        m2AudioDTSSource >> Initialization
-----------------------------------------------------------------------------*/

// Constructor
m2AudioDTSSource::m2AudioDTSSource(const char *FileName, int Downmix, int DRC, IScriptEnvironment *env)
{
        int Channels;   // channel count
        int Sync;       // synch info
        int _DecFlags;  // temporary flags
        int i;          // aux

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
                env->ThrowError("m2AudioDTSSource: unable to open file \"%s\"", StreamName);

        _fseeki64(Stream, 0, SEEK_END);                 // instead vfc for >4 GB size
        StreamLength = _ftelli64(Stream);               // moved, needed to dont pass the oef
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
        // Set decoder acceleration
        Accel = 0;
        // Find first frame
        State       = dts_init(Accel);
        FrameLength = MAX_DETECT_ERROR;
        Sync = Synchronize(FrameLength, Flags, Samplerate, Bitrate);
        if (Sync <= 0)
                env->ThrowError("m2AudioDTSSource: \"%s\" is not a valid DTS file", StreamName);

        StreamOffset = Sync - 1;          // StreamOffset = (Sync - 1) % FrameLength;
        _fseeki64(Stream, StreamOffset, SEEK_SET);

        // Initialize raw buffer
        Frame      = new unsigned char[FrameLength];
        FrameIndex = -2;

        // Get number of blocks per frame
        fread(Frame, 1, FrameLength, Stream);

        if (dts_frame(State, Frame, &Flags, &Level, 0))
                env->ThrowError("m2AudioDTSSource: error in file \"%s\"", StreamName);

        BlocksPerFrame  = dts_blocks_num(State);
        SamplesPerFrame = BlocksPerFrame * SAMPLES_PER_BLOCK;
        dts_free(State);
        State = 0;

        FrameCount = int((StreamLength - StreamOffset) / FrameLength);
// New code to detect padd or HD subframes @
        FrameExt = 0;

        fread(Frame, 1, 9, Stream);
        switch (((Frame)[3]<<24)|((Frame)[2]<<16)|((Frame)[1]<<8)|((Frame)[0]&0xff)) {
        case 25230975:                                  // Core only 0x0180FE7F 16_BE
        case 15269663:                                  // Core only 0x00E8FF1F 14_BE
        case -402644993:                                // Core only 0xE8001FFF 14_LE
        case -2147385346:                               // Core only 0x80017FFE 16_LE
                break;
        case 0:                                         // pad
                FrameExt = 9;
                Sync = 0;
                while (Sync == 0) {
                    if (FrameLength != fread(Frame, 1, FrameLength, Stream))
                        env->ThrowError("m2AudioDTSSource: \"%s\" is not a valid DTS file", StreamName);
                    for (i = 0; (i < FrameLength) && (Sync == 0); i++ )
                        if (0 == (Frame)[i]) FrameExt++;
                        else Sync = 1;
                }
                FrameCount = int((StreamLength - StreamOffset) / (FrameLength + FrameExt));
                break;
        case 622876772:                                 // HD subframe
                FrameExt = 1 + ((((Frame)[6]&0x0f)<<11)|((Frame)[7]<<3)|((Frame)[8]>>5));
                switch (((Frame)[4]<<12)|((Frame)[5]<<4)|((Frame)[6]>>4)) {
                case 54:                                // Hi Res (constant bitrate)
                    FrameCount = int((StreamLength - StreamOffset) / (FrameLength + FrameExt));
                    break;
                default:                // VBR file, we need read the whole file to know the core FrameCount
                  FrameCount = 1;                                                                       // vbr
                  _fseeki64(Stream, FrameExt - 9, SEEK_CUR);                                            // vbr
                                                                                                        // vbr
                  while (9 == fread(Frame, 1, 9, Stream)) {                                             // vbr
                      switch (((Frame)[3]<<24)|((Frame)[2]<<16)|((Frame)[1]<<8)|((Frame)[0]&0xff)) {    // vbr
                      case 25230975:                                  // Core                           // vbr
                          if ((StreamLength - _ftelli64(Stream)) >= (FrameLength - 9)) FrameCount++;    // vbr
                          _fseeki64(Stream, FrameLength - 9, SEEK_CUR);  // if ++                       // vbr
                          break;                                                                        // vbr
                      case 622876772:                                 // HD subframe                    // vbr
                          FrameExt = 1 + ((((Frame)[6]&0x0f)<<11)|((Frame)[7]<<3)|((Frame)[8]>>5));     // vbr
                          _fseeki64(Stream, FrameExt - 9, SEEK_CUR);                                    // vbr
                          break;                                                                        // vbr
                      default:                                                                          // vbr
                         _fseeki64(Stream, - 8, SEEK_CUR);                                              // vbr
                      }                                                                                 // vbr
                  }                                                                                     // vbr
                  FrameExt = -1;  // VBR signal                                                         // vbr
                }
                break;
        default:
                env->ThrowError("m2AudioDTSSource: unknow DTS format in file \"%s\"", StreamName);
        }
// End new code with FrameExt and FrameCount data

        _fseeki64(Stream, StreamOffset, SEEK_SET);

        // Set sample type
        Info.sample_type = SAMPLE_FLOAT;

        // Get channel count
        DecFlags = Flags;                          // flags = acmod + 8*dsurmod + 16*lfeon
        LFE = (0!=(Flags & DTS_LFE)); // WAZ Flags && DTS_LFE !!!!
        Channels = Flags & DTS_CHANNEL_MASK;

        switch (Channels) {
        case DTS_MONO:
                ChannelCount = 1;
                break;
        case DTS_CHANNEL:
        case DTS_STEREO:
        case DTS_STEREO_SUMDIFF:
        case DTS_STEREO_TOTAL:
        case DTS_DOLBY:
                ChannelCount = 2;
                break;
        case DTS_3F:
        case DTS_2F1R:
                ChannelCount = 3;
                break;
        case DTS_3F1R:
        case DTS_2F2R:
                ChannelCount = 4;
                break;
        case DTS_3F2R:
                ChannelCount = 5;
                break;
        default:
                env->ThrowError("m2AudioDTSSource: unsupported channel configuration in file \"%s\"", StreamName);
        }
        if (LFE) ChannelCount++;

        if (Downmix) {
                // Downmix
                switch (Downmix) {
                case 1:                         // 1 channel
                        _DecFlags = DTS_MONO;
                        break;

                case 2:                         // 2 channels
                        _DecFlags = DTS_STEREO;
                        break;

                case 4:                         // 4 channels
                        _DecFlags = DTS_2F2R;
                        break;

                case 6:                         // 6 channels
                        _DecFlags = DTS_3F2R;
                        if (LFE) _DecFlags |= DTS_LFE;
                        break;

                default:
                        env->ThrowError("m2AudioDTSSource: invalid number of channels requested");
                }

                if (Downmix < ChannelCount) {
                        ChannelCount = Downmix;
                        DecFlags     = _DecFlags;
                }
        }

        Info.nchannels = ChannelCount;

        // Remapping Matrix
        for (Channels=0; Channels < ChannelCount; Channels++) map[Channels] = Channels;
        Channels = DecFlags & 15;
        if (LFE) Channels += 16;

        switch (Channels) {                                    //amod + 16*LFE
//      dts output order is: front center, front left, front right, rear left, rear right, LFE
//      case 0:      // 0                1 channel                              ok
//      case 2:      // 0, 1             2 chan: left, right                    ok
//      case 6:      // 0, 1, 2          2/1 chan: FLeft, FRight, RCenter       ok
//      case 8:      // 0, 1, 2, 3       2/2 chan: FLeft, FRight, RLeft, RRight ok
//      case 16:     // 0, 1             2 chan: 1 mono + LFE ok                ok
//      case 18:     // 0, 1, 2          2/0.1 chan: left, right + LFE          ok
        case 25:     // 2, 0, 1, 4, 5, 3 3/2.1 chan: FLeft, FRight, FCenter + LFE, RLeft, RRight
               map[3] = 4;
               map[4] = 5;
               map[5] = 3;
        case 5:      // 2, 0, 1          3/0 chan: FLeft, FRight, FCenter
        case 7:      // 2, 0, 1, 3       3/1 chan: FLeft, FRight, FCenter, RCenter
        case 9:      // 2, 0, 1, 3, 4    3/2 chan: FLeft, FRight, FCenter, RLeft, RRight
        case 21:     // 2, 0, 1, 3       3/0.1 chan: FLeft, FRight, FCenter + LFE
               map[0] = 2;
               map[1] = 0;
               map[2] = 1;
               break;
        case 22:     // 0, 1, 3, 2       2/1.1 chan: FLeft, FRight + LFE, RCenter
               map[2] = 3;
               map[3] = 2;
               break;
        case 23:     // 2, 0, 1, 4, 3    3/1.1 chan: FLeft, FRight, FCenter + LFE, RCenter
               map[0] = 2;
               map[1] = 0;
               map[2] = 1;
               map[3] = 4;
               map[4] = 3;
               break;
        case 24:     // 0, 1, 3, 4, 2    2/2.1 chan: FLeft, FRight + LFE, RLeft, RRight
               map[2] = 3;
               map[3] = 4;
               map[4] = 2;
        }

        // Enable automatic level adjustment
        DecFlags |= DTS_ADJUST_LEVEL;
        Level     = 1;

        // Get sampling rate
        Info.audio_samples_per_second = Samplerate;

        // Get number of frames (now before new code)
        // FrameCount = int((StreamLength - StreamOffset) / FrameLength);

        // Get number of samples
        SampleCount            = __int64(FrameCount) * __int64(SamplesPerFrame);
        Info.num_audio_samples = SampleCount;

        // Initialize framebuffer
        BufferSize  = Info.nchannels * SamplesPerFrame;
        Buffer      = new float[BufferSize];
        BufferStart = BufferEnd = -1;
}

// Destructor
m2AudioDTSSource::~m2AudioDTSSource()
{
        // Close file
        if (Stream)
                fclose(Stream);

        // Free memory
        if (State)
                dts_free(State);

        if (Frame)
                delete[] Frame;

        if (Buffer)
                delete[] Buffer;
}

/*-----------------------------------------------------------------------------
        m2AudioDTSSource >> IClip interface
-----------------------------------------------------------------------------*/

// Return VideoInfo
const VideoInfo& __stdcall m2AudioDTSSource::GetVideoInfo()
{
        return Info;
}

// Return video frame
PVideoFrame __stdcall m2AudioDTSSource::GetFrame(int n, IScriptEnvironment *env)
{
        return NULL;
}

// Return video field parity
bool __stdcall m2AudioDTSSource::GetParity(int n)
{
        return false;
}

// Return audio samples
void __stdcall m2AudioDTSSource::GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env)
{
        __int64 i;                      // cycle counter
        int     _FrameIndex;            // index of the previous frame
        int     ReadOK;                 // result of ReadFrame
        float   *Input, *Output;        // buffer pointers
        __int64  Left;                  // number of samples to be copied from the buffer
        int     _FrameExt;              // aux for length of VBR HD subframes @ // vbr

//Code to prohibit illegal audio requests. Init buffer in that case
        i = ChannelCount * sizeof(float);               // BlockAlign
        if ( start >= SampleCount) {                    // No scan over max position.
                memset((char*)buf, 0, count * i);       // init buffer
                return;
        }

        if ( (start + count) >= SampleCount ) {         // No scan over max position.
                memset((char*)buf, 0, count * i);       // init buffer
                count = SampleCount - start;
        }

        if ( start + count <= 0 ) {                     // No requests before 0 possible.
                memset((char*)buf, 0, count * i);       // init buffer
                return;
        }

        if ( start < 0 ) {      //start filling the buffer later as requested. e.g. if start=-1 then omit 1 sample.
                memset((char*)buf, 0, count * i);       //init buffer
                count = count + start;                  //request fewer samples
                buf = (char*)buf - start * i;           //move buffer
                start = 0;
        }

        // Check if we must read a frame
        if (start < BufferStart || start > BufferEnd) {
                // Calculate the index and the sample range of the required frame
                _FrameIndex = FrameIndex;
                FrameIndex  = int(start / SamplesPerFrame);
                BufferStart = __int64(FrameIndex) * __int64(SamplesPerFrame);
                BufferEnd   = BufferStart + SamplesPerFrame;

                // Seek to the frame, if required
                if (FrameIndex != _FrameIndex + 1) {

                   // Reset decoder
                   if (State) dts_free(State);
                   State = dts_init(Accel);

                   if (FrameIndex == 0)
                       _fseeki64(Stream, StreamOffset, SEEK_SET);
                   else
                   {
// new code to search a frame (from begining of file if VBR frames) @
                       if (FrameExt < 0) {                                                                              // vbr
                           _FrameIndex = 0;                                                                             // vbr
                           _fseeki64(Stream, StreamOffset, SEEK_SET);                                                   // vbr
                                                                                                                        // vbr
                           while ((9 == fread(Frame, 1, 9, Stream)) && (_FrameIndex < FrameIndex)) {                    // vbr
                                   switch (((Frame)[3]<<24)|((Frame)[2]<<16)|((Frame)[1]<<8)|((Frame)[0]&0xff)) {       // vbr
                                   case 25230975:                                  // Core                              // vbr
                                           _FrameIndex++;                                                               // vbr
                                           _fseeki64(Stream, FrameLength - 9, SEEK_CUR);                                // vbr
                                           break;                                                                       // vbr
                                   case 622876772:                                 // HD subframe                       // vbr
                                           _FrameExt = 1 + ((((Frame)[6]&0x0f)<<11)|((Frame)[7]<<3)|((Frame)[8]>>5));   // vbr
                                           _fseeki64(Stream, _FrameExt - 9, SEEK_CUR);                                  // vbr
                                           break;                                                                       // vbr
                                   default:                                                                             // vbr
                                           _fseeki64(Stream, - 8, SEEK_CUR);                                            // vbr
                                   }                                                                                    // vbr
                           }                                                                                            // vbr
                           _fseeki64(Stream, - FrameLength, SEEK_CUR);                                                  // vbr
                       } else                                                                                           // vbr
// end new code
                           _fseeki64(Stream, StreamOffset + __int64(FrameIndex - 1) * __int64(FrameLength + FrameExt), SEEK_SET);

                           // Read previous frame to initialize the desired frame
                           if (!ReadFrame())
                                   env->ThrowError("m2AudioDTSSource: error 0 in file \"%s\"", StreamName);
                   }
                }

                // Read the frame
                ReadOK = ReadFrame();
                if (ReadOK == 0)
                        env->ThrowError("m2AudioDTSSource: error 1 in file \"%s\"", StreamName);
                if (ReadOK < 0) {              // EOF reached
                        SampleCount            = BufferEnd;       // return with a empty frame
                        Info.num_audio_samples = SampleCount;
                        count                  = BufferEnd - start;
                }
        }

        // Start decoding
        Input  = Buffer + int(start - BufferStart) * ChannelCount;
        Output = (float *)buf;
        Left   = SamplesPerFrame - (start - BufferStart);

        while (count) {
                // Copy samples
                if (count < Left) Left = count;
                count -= Left;

                for (i = 0; i < Left * ChannelCount; i++)
                        *Output++ = *Input++;

                // Check if we must read a frame
                if (count) {
                        // Read the frame
                        FrameIndex++;
                        ReadOK = ReadFrame();
                        if (ReadOK == 0)
                                env->ThrowError("m2AudioDTSSource: error 2 in file \"%s\"", StreamName);
                        if ((ReadOK < 0) && (count > SamplesPerFrame)) {  // EOF reached, return with a empty frame
                                SampleCount = __int64(FrameIndex + 1) * __int64(SamplesPerFrame);
                                Info.num_audio_samples = SampleCount;
                                count = SamplesPerFrame;
                        }

                        // Reset input buffer pointer
                        Input = Buffer;
                        Left  = SamplesPerFrame;
                }
        }

        // Calculate the sample range of the current frame
        BufferStart = __int64(FrameIndex) * __int64(SamplesPerFrame);
        BufferEnd   = BufferStart + SamplesPerFrame;
}

// Set cache hints
void __stdcall m2AudioDTSSource::SetCacheHints(int cachehints, int frame_range)
{
}

/*-----------------------------------------------------------------------------
        m2AudioDTSSource >> Operations
-----------------------------------------------------------------------------*/

// Synchronize with the audio stream
int m2AudioDTSSource::Synchronize(int& Length, int& Flags, int& Samplerate, int& Bitrate) {
        int Delta = 0;                                  // number of bytes out of synch
        int _Length;                                    // backup of Length
        unsigned char Buffer[FRAME_HEADER_LEN];         // buffer for a frameheader

        // Backup Length
        _Length = Length;

        // Try synchronization
        while (Delta < _Length - 1) {
                // Read header
                if (fread(Buffer, 1, FRAME_HEADER_LEN, Stream) != FRAME_HEADER_LEN)
                        return -1;

                // Get information
                Length = dts_syncinfo(State, Buffer, &Flags, &Samplerate, &Bitrate, &Length);

                if (Length) {                        // Success
                        _fseeki64(Stream, -FRAME_HEADER_LEN, SEEK_CUR);
                        return (Delta + 1);
                } else {                             // Try again
                        _fseeki64(Stream, 1 - FRAME_HEADER_LEN, SEEK_CUR);
                        Delta++;
                }
        }
        // Failure
        Length = _Length;
        return 0;          // Without a header in Length bytes
}

// Reads the next frame from the stream and decodes it into the framebuffer
int m2AudioDTSSource::ReadFrame() {
        int Sync;                               // synch info
        int Length = 0;                         // length of the current frame
        int _Flags, _Samplerate, _Bitrate;      // bit stream information

        while (!Length) {
               // Read next frame from stream
               if (fread(Frame, 1, FrameLength, Stream) != FrameLength) {
                       // Error at the end of the stream, mute frame
                       EmptyFrame();
                       return -1;     // When extract core there are many EmptyFrame at end of file we need break
               }

               // Get frame information
               Length = dts_syncinfo(State, Frame, &_Flags, &_Samplerate, &_Bitrate, &Length);

               // Check if we need synchronization
               if (!Length) {
                       // Seek back in the stream

                       _fseeki64(Stream, 1 - FrameLength, SEEK_CUR);

                       // Try synchronization
                       Length = FrameLength;
                       Sync   = Synchronize(Length, _Flags, _Samplerate, _Bitrate);

                       if (Sync < 0) {         // EOF reached, mute frame and exit
                             EmptyFrame();
                             return -1;
                       }
                       if (!Sync) Length = 0;  // Try another time to decode only the core ignoring extra data or padd bytes
               }
        }

        // Check if the frame has proper length and BSI
        if ((_Flags & 0x3F ) > 0) {
            if ((_Flags & 0x3F ) < 5)
                 _Flags = ( _Flags & 0xFFF8) | 0x02 ;
        }
        if ((Length != FrameLength) || (_Flags != Flags) || (_Samplerate != Samplerate))
                // Fatal error, unable to continue decoding
                return 0;

        // Decode frame
        if (!DecodeFrame()) EmptyFrame();

// new code to skip padd and HD subframes @
        if (FrameExt < 0) {                                                                                 // vbr
            if (9 == fread(Frame, 1, 9, Stream))  {                                                         // vbr
                if ((((Frame)[3]<<24)|((Frame)[2]<<16)|((Frame)[1]<<8)|((Frame)[0]&0xff)) == 622876772) {   // vbr
                        Length = 1 + ((((Frame)[6]&0x0f)<<11)|((Frame)[7]<<3)|((Frame)[8]>>5));             // vbr
                        _fseeki64(Stream, Length - 9, SEEK_CUR);                                            // vbr
                } else                                                                                      // vbr
                        _fseeki64(Stream, - 9, SEEK_CUR);                                                   // vbr
            }                                                                                               // vbr
        }                                                                                                   // vbr
        if (FrameExt > 0) _fseeki64(Stream, FrameExt, SEEK_CUR);
        return 1;
}

// Decode frame from the raw buffer
bool m2AudioDTSSource::DecodeFrame()
{
        int    blk, i, j;                       // cycle counters
        float *Input, *Output;  // buffer pointers

        // Start decoding the frame
        if (dts_frame(State, Frame, &DecFlags, &Level, 0) || (dts_blocks_num(State) != BlocksPerFrame))
                return false;

        // Dynamic range compression
        if(0==_drc)
                dts_dynrng(State, 0, 0);

        // The decoded samples will be in the following sequencial order:
        //   front center, front left, front right, rear left, rear right, LFE
        // If one of the channels is not present, it is skipped and the following
        // channels are shifted accordingly. We have to put the channels in
        // the proper interleaved order, based on the requested channel config.

        Output = Buffer;  // Decode each block
        for (blk = 0; blk < BlocksPerFrame; blk++) {

            if (dts_block(State)) return false; // Decode block
            Input = dts_samples(State);         // Copy channel

            for (j = 0; j < ChannelCount; j++) {
                for (i = 0; i < SAMPLES_PER_BLOCK; i++) {
                      Output[i * ChannelCount + map[j]] = *Input++;     // mapping matrix

                }
            }
            Output += SAMPLES_PER_BLOCK * ChannelCount;                   // Next block
        }

        return true;
}

// Empty the framebuffer
void m2AudioDTSSource::EmptyFrame()
{
        for (int i = 0; i < SamplesPerFrame * ChannelCount; i++)
                Buffer[i] = 0;
}

/*-----------------------------------------------------------------------------
        AviSynth plugin initialization
-----------------------------------------------------------------------------*/
/*
extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2(IScriptEnvironment *env)
{
        env->AddFunction("m2AudioDTSSource", "s[channels]i", m2AudioDTSSource::Create, 0);

        return "`m2audio_dts' m2Audio DTS Decoder Plugin";
}*/
