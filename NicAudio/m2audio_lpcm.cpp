#pragma warning(disable:4996)
/*=============================================================================
        Module Name:
                        m2audio_lpcm.cpp

        Abstract:
                        m2Audio Linear PCM decoder plugin for AviSynth 2.5

        License:
            GNU General Public License (GPL)

        Revision History:
                        * Oct  3, 2004:  Created by Attila Afra
                        * Jan  8, 2005:  Support for all formats
                        * Feb 13, 2005:  Minor changes
                        * Aug 21, 2007:  Bug fixes, remove restrictions, generalize code. IanB
                        * Feb 29, 2008:  Cancel obsolete code. Support for > 4 GB files. Map channels for new mode IsBluRay. Bugfix for count>Left. Tebasuna
------------------------------------------------------------------------------
        Copyright © 2004-2005 Attila T. Afra. All rights reserved.
=============================================================================*/

// Includes
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>
// #include "vfm.h"
#include "avisynth.h"
#include "AudioHeader.h"

/*-----------------------------------------------------------------------------
        Constants
-----------------------------------------------------------------------------*/

#define FRAME_LENGTH    50400                   // length of a frame (raw). Guarantee until 32 bits 8 channels multiple

/*=============================================================================
        m2AudioLPCMSource >> Implementation
=============================================================================*/

AVSValue __cdecl m2AudioLPCMSource::Create(AVSValue args, void *, IScriptEnvironment *env)
{       //                           FileName,           SampleRate,      SampleBits,      Channels
        return new m2AudioLPCMSource(args[0].AsString(""), args[1].AsInt(0), args[2].AsInt(0), args[3].AsInt(0), env);
}

/*-----------------------------------------------------------------------------
        m2AudioLPCMSource >> Initialization
-----------------------------------------------------------------------------*/

// Constructor
m2AudioLPCMSource::m2AudioLPCMSource(const char *FileName, int SampleRate, int SampleBits, int Channels, IScriptEnvironment *env)
{
        // Initialize pointers
        Stream                  = NULL;
        Frame                   = 0;
        Buffer                  = 0;
        bIsBluRay               = false;

        // Empty information structure
        memset(&Info, 0, sizeof(VideoInfo));

        // Store filename
        StreamName = new char[strlen(FileName) + 1];
        strcpy(StreamName, FileName);

        // Open file
        if (!(Stream = fopen(StreamName, "rb")))
                env->ThrowError("m2AudioLPCMSource: unable to open file \"%s\"", StreamName);

        // Check

        if ((Channels < 1) || (Channels > 8))
                env->ThrowError("m2AudioLPCMSource: unsupported number of channels defined");

        // Set channel count
        ChannelCount = Channels;

        // Other kind of lpcm with simple big-endian order and channels remapped
        if ( SampleBits < 0 ) {
                bIsBluRay = true;
                SampleBits = 0 - SampleBits;     // Valid parameter for BluRay: -16, -24, -32 (?)
                Quant = SampleBits / 8;          // Quant only auxiliar here to prepare the matrix
                // Initializing remap matrix
                for (Channels=0; Channels < ChannelCount; Channels++) map[Channels] = (Channels + 1) * Quant;
                if (ChannelCount > 5) {
                         map[ChannelCount - 1] = 4 * Quant;            // LFE from last to fourth
                         map[3]                = 5 * Quant;            // BL to fifth
                         map[ChannelCount - 2] = 6 * Quant;            // BR from penultimate to sixth
                }
                if (ChannelCount > 6)   map[4] = 7 * Quant;            // SL or BC
                if (ChannelCount > 7)   map[5] = 8 * Quant;            // SR
        }

        // Set sample type
        InQuant = SampleBits;

        switch (InQuant)
        {
        // 16 bit
        case 16:
                Quant = InQuant;
                Info.sample_type = SAMPLE_INT16;
                break;
        // 20 bit
        case 20:                  // verify ChannelCount even (if mono InBlock = 1 * 20 / 8 = 2.5 ??)
                if ( bIsBluRay || ( ChannelCount != 2 * int(ChannelCount / 2)))
                        env->ThrowError("m2AudioLPCMSource: 20 bit unsupported in this context.");
        // 24 bit
        case 24:
                Quant = 24;
                Info.sample_type = SAMPLE_INT24;
                break;
        // 32 bit
        case 32:
                Quant = 32;
                Info.sample_type = SAMPLE_INT32;
                break;
        default:
                env->ThrowError("m2AudioLPCMSource: unsupported sample precision defined");
        }

        // Set channel info
        Info.nchannels = ChannelCount;

        // Set info sampling rate
        Info.audio_samples_per_second = SampleRate;

	// Calculate number of samples per channel
        _fseeki64(Stream, 0, SEEK_END);                 // instead vfc for >4 GB size
        StreamLength = _ftelli64(Stream);
        _fseeki64(Stream, 0, SEEK_SET);

	InBlock = (ChannelCount * InQuant) / 8;
	SampleCount = StreamLength / InBlock;                 // Truncate! (if incomplete last sample) ok
	Info.num_audio_samples = SampleCount;

	// Calculate number of samples per channel per frame
	SamplesPerFrame = FRAME_LENGTH / InBlock;             // always exact with 50400

	// Initialize buffers
	Frame = new unsigned char[FRAME_LENGTH];
	FrameIndex = -2;

	// We need auxiliary buffer because we cannot copy the samples directly from the raw buffer
        OutBlock = (ChannelCount * Quant) / 8;                       // output bytes per sample, only different
	Buffer = new unsigned char[SamplesPerFrame * OutBlock];      // from InBlock if 20 -> 24 bits
	BufferStart = BufferEnd = -1;

}

// Destructor
m2AudioLPCMSource::~m2AudioLPCMSource()
{
        // Close file
        if (Stream)
                fclose(Stream);

        // Free memory
        if (StreamName)
                delete[] StreamName;

        if (Frame)
                delete[] Frame;

        if (Buffer)
                delete[] Buffer;

}

/*-----------------------------------------------------------------------------
        m2AudioLPCMSource >> IClip interface
-----------------------------------------------------------------------------*/

// Return VideoInfo
const VideoInfo& __stdcall m2AudioLPCMSource::GetVideoInfo()
{
        return Info;
}

// Return video frame
PVideoFrame __stdcall m2AudioLPCMSource::GetFrame(int n, IScriptEnvironment *env)
{
        return NULL;
}

// Return video field parity
bool __stdcall m2AudioLPCMSource::GetParity(int n)
{
        return false;
}

// Return audio samples
void __stdcall m2AudioLPCMSource::GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env)
{
        int            _FrameIndex;                     // index of the previous frame
        unsigned char *Input, *Output;                  // buffer pointers
        __int64        Left;                            // number of samples to be copied from the buffer

//Code to prohibit illegal audio requests. Init buffer in that case
        if ( start >= Info.num_audio_samples) {   // No scan over max position.
                memset((char*)buf, 0, unsigned int(count * Info.BytesPerAudioSample()));        //init buffer
                return;
        }

        if ( (start + count)>= Info.num_audio_samples ) {  // No scan over max position.
                memset((char*)buf, 0, unsigned int(count * Info.BytesPerAudioSample()));        //init buffer
                count = Info.num_audio_samples - start;
        }

        if ( start + count <=0 ) { // No requests before 0 possible.
                memset((char*)buf, 0, unsigned int(count * Info.BytesPerAudioSample()));        //init buffer
                return;
        }

        if ( start < 0 ) {      //start filling the buffer later as requested. e.g. if start=-1 then omit 1 sample.
                memset((char*)buf, 0, unsigned int(-start * Info.BytesPerAudioSample()));       //init buffer
                buf = (char*)buf - start * Info.BytesPerAudioSample();  //move buffer
                count += start; //request fewer samples
                start = 0;
        }

        // Check if we must read a frame
        if (start < BufferStart || start > BufferEnd) {
                // Calculate the index and the sample range of the required frame
                _FrameIndex = FrameIndex;
                FrameIndex  = int(start / (FRAME_LENGTH / InBlock));
                BufferStart = __int64(FrameIndex) * (FRAME_LENGTH / InBlock);

                // Seek to the frame, if required
                if (FrameIndex != _FrameIndex + 1) {
                        _fseeki64(Stream, __int64(FrameIndex) * FRAME_LENGTH, SEEK_SET);
                }

                // Read the frame
                if (!ReadFrame()) env->ThrowError("m2AudioLPCMSource: error in file \"%s\"", StreamName);
        }

        // Start decoding
        Input = Buffer + int(start - BufferStart) * OutBlock;
        Output = (unsigned char *)buf;

        Left = SamplesPerFrame - (start - BufferStart);

        while (count) {
                // Copy samples
                if (count < Left) Left = count;
                memcpy(Output, Input, unsigned int(Left * OutBlock));
                Output += (Left * OutBlock);

                count -= Left;
                // Check if we must read a frame
                if (count) {
                        // Read the frame
                        FrameIndex++;
                        if (!ReadFrame()) env->ThrowError("m2AudioLPCMSource: error in file \"%s\"", StreamName);

                        // Reset input buffer pointer
                        Input = Buffer;
                        Left  = SamplesPerFrame;
                }
        }

        // Calculate the sample range of the current frame
        BufferStart = __int64(FrameIndex) * (FRAME_LENGTH / InBlock);
        BufferEnd   = BufferStart         + SamplesPerFrame;
}

// Set cache hints
void __stdcall m2AudioLPCMSource::SetCacheHints(int cachehints, int frame_range)
{
}

/*-----------------------------------------------------------------------------
        m2AudioLPCMSource >> Operations
-----------------------------------------------------------------------------*/

// Reads the next frame from the stream and copies the samples into the buffer
bool m2AudioLPCMSource::ReadFrame()
{
        int            i, j, k, byt;         // counters
        unsigned char *Input, *Output;  // input/output buffer pointers

        // Read next frame from stream
        if ((FrameLength = fread(Frame, 1, FRAME_LENGTH, Stream)) < 0)
                // Read error
                return false;

        // Calculate number of samples per channel per frame
        SamplesPerFrame = FrameLength / InBlock;

        // Convert samples
        Input = Frame;
        Output = Buffer;

// Now simplified code
        if ( bIsBluRay ) {
           byt = InQuant / 8;
           for (i = 0; i < FrameLength; i += InBlock)
              for (k = 0; k < ChannelCount; k++)
                 for (j = 1; j < byt + 1; j++) {
                      Output[i + map[k] - j] = Input[0];
                      Input++;
                 }
        }
        else {
           switch (InQuant)
           {
           // 16 bit
           case 16:        // Swap bytes
                for (i = 0; i < FrameLength; i += 2)
                {
                        Output[i]     = Input[i + 1];
                        Output[i + 1] = Input[i];
                }
                break;

           // 20 bit
           case 20:
                // Convert to 24 bit
                for (i = 0; i < FrameLength / (5 * ChannelCount); i++)
                {
                        for (j = 0; j < ChannelCount; j++)
                        {
                                Output[0] = Input[4 * ChannelCount + j] & 0xf0;
                                Output[1] = Input[4 * j + 1];
                                Output[2] = Input[4 * j + 0];

                                Output[3] = Input[4 * ChannelCount + j] << 4;
                                Output[4] = Input[4 * j + 3];
                                Output[5] = Input[4 * j + 2];

                                Output += 6;
                        }
                        Input += 5 * ChannelCount;
                }
                break;

           // 24 bit
           case 24:
                for (i = 0; i < FrameLength / (6 * ChannelCount); i++)
                {
                     for (j = 0; j < 2 * ChannelCount; j++)
                     {                                                       // 4 channel              // 2 channel  // 1 channel
                             Output[0] = Input[4 * ChannelCount + j];        //16,17,18,19,20,21,22,23 // 8, 9,10,11 // 4, 5 // 4c, 4c+1, 4c+2, ..., 6c-1
                             Output[1] = Input[2 * j + 1];                   // 1, 3, 5, 7, 9,11,13,15 // 1, 3, 5, 7 // 1, 3 // 1, 3, 5, ..., 4c-1
                             Output[2] = Input[2 * j];                       // 0, 2, 4, 6, 8,10,12,14 // 0, 2, 4, 6 // 0, 2 // 0, 2, 4, ..., 4c-2

                             Output += 3;
                     }
                     Input += 6 * ChannelCount;
                }
                break;

           // 32 bit
           case 32:  // Intuit a hypothetical 32bit LPCM packing
                for (i = 0; i < FrameLength / (8 * ChannelCount); i++)
                {
                        for (j = 0; j < 2 * ChannelCount; j++)
                        {                                                         // 2 channel  // 1 channel
                                Output[0] = Input[4 * ChannelCount + 2 * j + 1];  // 9,11,13,15 // 5, 7 // 4c+1, 4c+3, 4c+5, ..., 8c-1
                                Output[1] = Input[4 * ChannelCount + 2 * j];      // 8,10,12,14 // 4, 6 // 4c+0, 4c+2, 4c+4, ..., 8c-2
                                Output[2] = Input[2 * j + 1];                     // 1, 3, 5, 7 // 1, 3 // 1, 3, 5, ..., 4c-1
                                Output[3] = Input[2 * j];                         // 0, 2, 4, 6 // 0, 2 // 0, 2, 4, ..., 4c-2

                                Output += 4;
                        }
                        Input += 8 * ChannelCount;
                }
                break;

           default:
                   return false; // Oops!
           }
        }

        // Success
        return true;
}

/*-----------------------------------------------------------------------------
        AviSynth plugin initialization
-----------------------------------------------------------------------------*/
/*
extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2(IScriptEnvironment *env)
{
        env->AddFunction("m2AudioLPCMSource", "s[samplerate]i[samplebits]i[channels]i", m2AudioLPCMSource::Create, 0);

        return "`m2audio_lpcm' m2Audio LPCM Decoder Plugin";
}*/
