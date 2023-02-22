#pragma warning(disable:4244)
//#pragma warning(disable:4293)
#pragma warning(disable:4996)

/*=============================================================================
        Module Name:
                        m2audio_rawav.cpp

        Abstract:
                        m2Audio PCM decoder plugin for AviSynth 2.5

        License:
            GNU General Public License (GPL)

        Revision History:
                        * 2007-08-22
=============================================================================*/

// Includes
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>

#include "avisynth.h"
#include "AudioHeader.h"

/* Macros to read unsigned int from char buffer */
#define READ_U64_LE(aux) (((aux)[0]&0xff)|((aux)[1]<<8)|((aux)[2]<<16)|((aux)[3]<<24)|((__int64)((aux)[4])<<32)|((__int64)((aux)[5])<<40)|((__int64)((aux)[6])<<48)|((__int64)((aux)[7])<<56))
#define READ_U32_LE(aux) (((aux)[0]&0xff)|((aux)[1]<<8)|((aux)[2]<<16)|((aux)[3]<<24))
#define READ_U16_LE(aux) (((aux)[0]&0xff)|((aux)[1]<<8))

#define READ_U64_BE(aux) (((__int64)((aux)[0])<<56)|((__int64)((aux)[1])<<48)|((__int64)((aux)[2])<<40)|((__int64)((aux)[3])<<32)|((aux)[4]<<24)|((aux)[5]<<16)|((aux)[6]<<8)|((aux)[7]&0xff))
#define READ_U32_BE(aux) (((aux)[0]<<24)|((aux)[1]<<16)|((aux)[2]<<8)|((aux)[3]&0xff))
#define READ_U16_BE(aux) (((aux)[0]<<8)|((aux)[1]&0xff))

/*=============================================================================
        m2RaWavSource >> Implementation
=============================================================================*/

AVSValue __cdecl m2RaWavSource::Create(AVSValue args, void *, IScriptEnvironment *env) {
        return new m2RaWavSource(args[0].AsString(""), args[1].AsInt(0), args[2].AsInt(0), args[3].AsInt(0), env);
}

/*-----------------------------------------------------------------------------
        m2RaWavSource >> Initialization
-----------------------------------------------------------------------------*/

// Constructor
m2RaWavSource::m2RaWavSource(const char *FileName, int SampleRate, int SampleBits, int Channels, IScriptEnvironment *env) {
        // Initialize pointers
        Stream                = NULL;
        char szRIFF[5]        = "";
        unsigned char aux[20] = "";            //  aux buffer to read header
        HeaderSize            = 0;             //  New variable must be global see AudioHeader.h
        bool IsRF64           = false;         //  the stream is a RF64 file or CAF for DataSize 64 bits
        bool IsW64            = false;         //  the stream is a w64 file
        int AudioFormat;                       //  can be 1 for int or 3 for float in wav
        int IgnoreLength      = 2;             //  can be 1, 2 (default) or 4
        __int64 DataSize      = 0;             //  DataSize read, 64 bits in RF64, w64 and caf
        double SamRaFloat     = 0.0;           //  Samplerate in float for .caf

        // Empty information structure
        memset(&Info, 0, sizeof(VideoInfo));

        // Store filename
        StreamName = new char[strlen(FileName) + 1];
        strcpy(StreamName, FileName);

        // Open file
        if (!(Stream = fopen(StreamName, "rb")))
                env->ThrowError("m2RaWavSource: unable to open file \"%s\"", StreamName);

        _fseeki64(Stream, 0, SEEK_END);                 // instead vfc for >4 GB size
        StreamLength = _ftelli64(Stream);               // moved, needed to dont pass the oef
        _fseeki64(Stream, 0, SEEK_SET);

        if (SampleRate > 0) IgnoreLength = SampleRate;  // raw data with Samplerate 1..4 is not allowed

        if ( IgnoreLength < 5 ) {                // Is not raw data, then the parameter is IgnoreLength, SampleBits and Channels are ignored.
            SampleRate = 0;                      // To allow read Samplerate 1..4

            fread(szRIFF, 1, 4, Stream);
            szRIFF[4] = 0;

// WAV, BWF, W64, RF64 formats =====================================================================
            if ( strncmp(szRIFF, "riff", 4) == 0 ) IsW64 = true;    // "riff" for w64 header
            if ( strncmp(szRIFF, "RF64", 4) == 0 ) IsRF64 = true;   // "RF64" for RF64 header
            if ( (strncmp(szRIFF, "RIFF", 4) == 0) || (IsW64) || (IsRF64) ) {
                fread(aux, 1, (IsW64 ? 20 : 4), Stream);      // File length minus first 8 bytes of RIFF description, we don't use it
                                                              // 12 for the rest of RIFF-GUID, 8 for the filelength not used
                fread(szRIFF, 1, 4, Stream);
                if ( strnicmp(szRIFF, "WAVE", 4) == 0 ) {     // ignore case
                    HeaderSize = (IsW64 ? 40 : 12);

        // Search chunks 'fmt ', 'ds64' and ignore others until reach 'data' chunk
                    while ( (strncmp(szRIFF, "data", 4) != 0) && (HeaderSize < StreamLength) ) { // accept chunk "fact" and others
                        _fseeki64(Stream, HeaderSize, SEEK_SET);
                        fread(szRIFF, 1, 4, Stream);
                        if ( IsW64 ) fread(aux, 1, 12, Stream);   // 12 extra bytes in chunk-guid
                        fread(aux, 1, 4, Stream);                 // chunk_length, in W64 the chunk length include the chunk-guide-length
                        if ( strncmp(szRIFF, "data", 4) != 0 ) {  // continue searching, Point HeaderSize to next chunk (bytes read + chunk_length)
                             HeaderSize += 8 + READ_U32_LE(aux);
                             if ( IsW64 ) HeaderSize -= (8 - ((aux)[0]&0x07) );  // always 8 multiple
                        }
        // 'fmt ' chunk
                        if ( strncmp(szRIFF, "fmt ", 4) == 0 ) {
                             if ( IsW64 ) fread(aux, 1, 4, Stream);   // high bytes from fmt_length

              // Read important header fields              AudioFormat(0-1), NumChannels(2-3), Samplerate(4-7)
                            fread(aux, 1, 16, Stream); //  ByteRate(8-11), BlockAlign(12-13), BitsPerSample(14-15)
                            AudioFormat = READ_U16_LE(aux);
                            Channels    = READ_U16_LE(aux+2);
                            SampleRate  = READ_U32_LE(aux+4);
                            SampleBits  = READ_U16_LE(aux+14);

              // WAVE_FORMAT_EXTENSIBLE
                            if ( AudioFormat == 65534 ) {        // cbSize(0-1), ValidBitsPerSample(2-3)
                               fread(aux, 1, 10, Stream);        // ChannelMask(4-7), AudioFormat(8-9), rest of GUID not used
                               AudioFormat = READ_U16_LE(aux+8);
                            }
                        }
        // 'ds64' chunk in RF64
                        if ( strncmp(szRIFF, "ds64", 4) == 0 ) {

              // Read the DataSize 64                      RiffSize(1-7), DataSize(8-15), SampleCount(16-23)
                            fread(aux, 1, 16, Stream);
                            DataSize = READ_U64_LE(aux+8);
                        }
                    }

        // Checks
                    if (SampleRate < 1)
                            env->ThrowError("m2RaWavSource: Don't found subchunk 'fmt '");

                    if ( strncmp(szRIFF, "data", 4) != 0 )
                            env->ThrowError("m2RaWavSource: Don't found subchunk 'data'");

                    if ( (AudioFormat == 3) && (SampleBits == 32) ) // 3 for float
                            SampleBits = 33;                        // after recover the 32 bits and SAMPLE_FLOAT type
                    else if ( AudioFormat != 1 )                    // 1 for int
                            env->ThrowError("m2RaWavSource: unsupported Audio Format");

        // Values
                    HeaderSize += (IsW64 ? 24 : 8);      // Adjust to point to first audio data, chunk+length 8 bytes
                                                         // In W64 the the chunk-guide-length use 24 bytes
                    if ( DataSize == 0 ) {               // 'ds64' don't found then read from 'data' chunk
                         DataSize = READ_U32_LE(aux);
                         if ( IsW64 ) {                     // is a W64 with 8 bytes for length
                             fread(aux, 1, 4, Stream);      // Next 4 bytes in W64 data length
                             DataSize += ((__int64)(READ_U32_LE(aux))<<32);
                         }
                    }

                } else
                         env->ThrowError("m2RaWavSource: Don't found format 'WAVE'");

// AU ==============================================================================================
            } else if ( strncmp(szRIFF, ".snd", 4) == 0 ) {         // .au
                fread(aux, 1, 20, Stream);
                HeaderSize = READ_U32_BE(aux);
                DataSize   = READ_U32_BE(aux+4);
                SampleBits = READ_U32_BE(aux+8);
                SampleRate = READ_U32_BE(aux+12);
                Channels   = READ_U32_BE(aux+16);
                switch (SampleBits) {
                case 2:
                        SampleBits = 8;
                        break;
                case 3:
                        SampleBits = 16;
                        break;
                case 4:
                        SampleBits = 24;
                        break;
                case 5:
                        SampleBits = 32;
                        break;
                case 6:
                        SampleBits = 33;                 // after recover 32 and float
                        break;
                default:
                        env->ThrowError("m2RaWavSource: unsupported sample precision or u-law");
                }
// AIF =============================================================================================
            } else if ( strncmp(szRIFF, "FORM", 4) == 0 ) {       // .aif
                fread(aux, 1, 4, Stream);                         // File length

                fread(szRIFF, 1, 4, Stream);
                if ( strncmp(szRIFF, "AIFF", 4) == 0 ) {
                    HeaderSize = 12;

        // Search chunk 'COMM' and ignore others until reach 'SSND' chunk
                    while ( (strncmp(szRIFF, "SSND", 4) != 0) && (HeaderSize < StreamLength) ) { // accept others chunks
                        _fseeki64(Stream, HeaderSize, SEEK_SET);
                        fread(szRIFF, 1, 4, Stream);
                        fread(aux, 1, 4, Stream);                 // chunk_length
                        if ( strncmp(szRIFF, "SSND", 4) != 0 )    // continue searching, Point HeaderSize to next chunk (bytes read + fmt_length)
                             HeaderSize += 8 + READ_U32_BE(aux);
        // 'COMM' chunk
                        if ( strncmp(szRIFF, "COMM", 4) == 0 ) {

                            fread(aux, 1, 18, Stream); // Num_chan(0-1), numSampleFrames(2-5), SampleSize(6-7)
                            Channels    = READ_U16_BE(aux);          //  SampleRate(8-17) real extended
                            SampleBits  = READ_U16_BE(aux+6);
                            AudioFormat = aux[9];                    // aux to convert real extended
                            SampleRate  = READ_U16_BE(aux+10);
                            if (AudioFormat > 14) SampleRate <<= (AudioFormat - 14);  // hack for extended real (10 bytes) valid for SampleRate range.
                            else if (AudioFormat < 14) SampleRate >>= (14 - AudioFormat);
                            if ( (SampleBits != 8) &&  (SampleBits != 16) &&  (SampleBits != 24) &&  (SampleBits != 32) )
                                env->ThrowError("m2RaWavSource: unsupported sample precision");
                        }
                    }
        // Checks
                    if (SampleRate < 1)
                            env->ThrowError("m2RaWavSource: Don't found subchunk 'COMM'");

                    if ( strncmp(szRIFF, "SSND", 4) != 0 )
                            env->ThrowError("m2RaWavSource: Don't found subchunk 'SSND'");
        // Values
                    DataSize   = READ_U32_BE(aux);
                    HeaderSize += 16;
                } else
                         env->ThrowError("m2RaWavSource: Don't found format 'AIFF'");
// CAF =============================================================================================
            } else if ( strncmp(szRIFF, "caff", 4) == 0 ) {       // .caf
                    fread(aux, 1, 4, Stream);                     // version
                    HeaderSize = 8;

                    while ( (strncmp(szRIFF, "data", 4) != 0) && (HeaderSize < StreamLength) ) { // accept chunk "fact" and others
                        _fseeki64(Stream, HeaderSize, SEEK_SET);
                        fread(szRIFF, 1, 4, Stream);
                        fread(aux, 1, 8, Stream);                 // chunk_length
                        if ( strncmp(szRIFF, "data", 4) != 0 )    // continue searching, Point HeaderSize to next chunk (bytes read + fmt_length)
                             HeaderSize += 12 + READ_U64_BE(aux);
        // 'desc' chunk
                        if ( strncmp(szRIFF, "desc", 4) == 0 ) {

              // Read important header fields
                            fread(aux, 1, 8, Stream);

                            AudioFormat = READ_U16_BE(aux) - 16384;              // aux to convert real
                            SampleRate  = READ_U16_BE(aux+2);
                            SampleRate  >>= 5;
                            SampleRate  += 32768 + 2048 * (AudioFormat % 16);         // mantisa
                            AudioFormat >>= 4;                                        // exponent
                            if (AudioFormat > 14) SampleRate <<= (AudioFormat - 14);  // hack for real (8 bytes) valid for SampleRate range.
                            else if (AudioFormat < 14) SampleRate >>= (14 - AudioFormat);

                            fread(szRIFF, 1, 4, Stream);                              // lpcm
                            szRIFF[4] = 0;
                            if ( strncmp(szRIFF, "lpcm", 4) != 0 )
                               env->ThrowError("m2RaWavSource: Unssuported CAF format %s", szRIFF);
                            fread(aux, 1, 20, Stream);  // FormatFlags(0-3), BytesPerPacket(4-7), FramesPerPacket(8-11)
                            AudioFormat = aux[3];                 // 0 int, 1 float , > 1 LittleEndian
                            Channels   = READ_U16_BE(aux+14);     // ChannelsPerFrame(12-15)
                            SampleBits = READ_U16_BE(aux+18);     // BitsPerChannel(16-19)

                            if ( (AudioFormat == 1) && (SampleBits == 32) ) // float
                                    SampleBits = 33;                        // after recover the 32 bits and SAMPLE_FLOAT type
                            else if ( AudioFormat != 0 )                    // 0 for int
                                    env->ThrowError("m2RaWavSource: unsupported Audio Format");
                        }
                    }
        // Checks
                    if (SampleRate < 1)
                            env->ThrowError("m2RaWavSource: Don't found subchunk 'desc'");

                    if ( strncmp(szRIFF, "data", 4) != 0 )
                            env->ThrowError("m2RaWavSource: Don't found subchunk 'data'");
        // Values
                    DataSize   = READ_U64_BE(aux);
                    IsRF64 = true;                                 // Only for say DataSize is 64 bits
                    HeaderSize += 12;

// End formats =====================================================================================
            } else env->ThrowError("m2RaWavSource: Unssuported PCM format %s", szRIFF);

            _fseeki64(Stream, HeaderSize, SEEK_SET);         // generic fseek with HeaderSize

            StreamLength -= HeaderSize;                      // Audio Data Length until eof

        // IgnoreLength
            if ( StreamLength > DataSize ) {                 // Only this have sense
                switch (IgnoreLength) {
                case 1:                                      // Force Ignore Length even for w64, RF64 and CAF
                        break;                               // Audio Data Length until eof
                case 4:                                      // Ignore Length only if > 4 GB
                        if (StreamLength < 4294967296) {
                            StreamLength = DataSize;
                            break;
                        }
                default:                                     // any other value is assumed like 2
                        if ( (StreamLength < 2147483648) || (IsW64) || (IsRF64) ) // Ignore Length only if > 2 GB
                            StreamLength = DataSize;
                }
            }
        }
// RAW =============================================================================================

        // Generic Check
        if ((Channels < 1) || (Channels > 8))
                env->ThrowError("m2RaWavSource: unsupported number of channels defined");

        // Fill Info fields, set sample type (32 float is SampleBits == 33)
        switch (SampleBits) {

        case 8:
                Info.sample_type = SAMPLE_INT8;
                break;
        case 16:
                Info.sample_type = SAMPLE_INT16;
                break;
        case 24:
                Info.sample_type = SAMPLE_INT24;
                break;
        case 32:
                Info.sample_type = SAMPLE_INT32;
                break;
        case 33:
                Info.sample_type = SAMPLE_FLOAT;
                SampleBits = 32;
                break;
        default:
                env->ThrowError("m2RaWavSource: unsupported sample precision");
        }

        // Set channels
        Info.nchannels = Channels;

        // Set sampling rate
        Info.audio_samples_per_second = SampleRate;

        // Calculate BlockAlign needed in others routines instead Channels and SampleBits
        BlockAlign  = Channels * SampleBits / 8;

        // Calculate and set the number of audio samples
        SampleCount = StreamLength / BlockAlign;
        Info.num_audio_samples = SampleCount;
}

// Destructor
m2RaWavSource::~m2RaWavSource() {
        // Close file
        if (Stream) fclose(Stream);
}

/*-----------------------------------------------------------------------------
        m2RaWavSource >> IClip interface
-----------------------------------------------------------------------------*/

// Return VideoInfo
const VideoInfo& __stdcall m2RaWavSource::GetVideoInfo() {
        return Info;
}

// Return video frame
PVideoFrame __stdcall m2RaWavSource::GetFrame(int n, IScriptEnvironment *env) {
        return NULL;
}

// Return video field parity
bool __stdcall m2RaWavSource::GetParity(int n) {
        return false;
}

// Return audio samples
void __stdcall m2RaWavSource::GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env) {

  //Code to prohibit illegal audio requests. Init buffer in that case
        if ( start >= SampleCount) {                        // No scan over max position.
                memset((char*)buf, 0, count * BlockAlign);  //init buffer
                return;
        }

        if ( (start + count) >= SampleCount ) {             // No scan over max position.
                memset((char*)buf, 0, count * BlockAlign);  //init buffer
                count = SampleCount - start;
        }

        if ( start + count <= 0 ) {                         // No requests before 0 possible.
                memset((char*)buf, 0, count * BlockAlign);  //init buffer
                return;
        }

        if ( start < 0 ) {      //start filling the buffer later as requested. e.g. if start=-1 then omit 1 sample.
                memset((char*)buf, 0, count * BlockAlign);  //init buffer
                count = count + start;                      //request fewer samples
                buf = (char*)buf - start * BlockAlign;      //move buffer
                start = 0;
        }

  //start of decoding
        _fseeki64(Stream, start * BlockAlign + HeaderSize, SEEK_SET);    //@
        fread( buf, 1, count * BlockAlign, Stream);
}


// Set cache hints
void __stdcall m2RaWavSource::SetCacheHints(int cachehints, int frame_range) {
}

/*-----------------------------------------------------------------------------
        AviSynth plugin initialization
-----------------------------------------------------------------------------*/
/*
extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2(IScriptEnvironment *env) {

        env->AddFunction("RaWavSource", "s[samplerate]i[samplebits]i[channels]i", m2RaWavSource::Create, 0);

        return "Audio PCM reader Plugin";
}*/
