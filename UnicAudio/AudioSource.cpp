#pragma warning(disable:4244)
#pragma warning(disable:4996)
// #pragma once
/*
** AudioSource. version 1.0
** AviSynth Plugin for AviSynth 2.0x based on mpg123, liba52 and libdts
** to unify different functions of NicAudio (v2.0.2)
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

// include generic
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>

// include specific
#include "avisynth.h"
//#include "AudioHeader.h"

// include libs
#include "mpg123\interface.h"

extern "C" {
#include "liba52\a52.h"
#include "liba52\a52_internal.h"

#include "libdts\dts.h"
#include "libdts\dts_internal.h"
}

// define mpa
#define INBUFSIZE 16384
#define OUTBUFSIZE 8192

// define dts
#define SAMPLES_PER_BLOCK       256        // number of samples per channel in a block, dts and ac3
#define FRAME_HEADER_LED        14         // length of a DTS frame's header

// define ac3
#define BLOCKS_PER_FRAME        6          // number of blocks in a frame
#define FRAME_HEADER_LEN        7          // length of a AC3 frame's header

// define lpcm
#define FRAME_LENGTH    50400              // length of a frame (raw). Guarantee until 32 bits 8 channels multiple

/* Macros to read unsigned int from char buffer */
#define READ_U64_LE(aux) (((aux)[0]&0xff)|((aux)[1]<<8)|((aux)[2]<<16)|((aux)[3]<<24)|((__int64)((aux)[4])<<32)|((__int64)((aux)[5])<<40)|((__int64)((aux)[6])<<48)|((__int64)((aux)[7])<<56))
#define READ_U32_LE(aux) (((aux)[0]&0xff)|((aux)[1]<<8)|((aux)[2]<<16)|((aux)[3]<<24))
#define READ_U16_LE(aux) (((aux)[0]&0xff)|((aux)[1]<<8))

#define READ_U64_BE(aux) (((__int64)((aux)[0])<<56)|((__int64)((aux)[1])<<48)|((__int64)((aux)[2])<<40)|((__int64)((aux)[3])<<32)|((aux)[4]<<24)|((aux)[5]<<16)|((aux)[6]<<8)|((aux)[7]&0xff))
#define READ_U32_BE(aux) (((aux)[0]<<24)|((aux)[1]<<16)|((aux)[2]<<8)|((aux)[3]&0xff))
#define READ_U16_BE(aux) (((aux)[0]<<8)|((aux)[1]&0xff))

//---------------------------------------------------------------------------------------
// 1) DEFINITIONS
//---------------------------------------------------------------------------------------
class UnicSource : public IClip {
    VideoInfo     Info;                    // required by AviSynth
    int           tipo;                    // mpa=1, dts=2, ac3=3, lpcm=4, rawav=5

// static variables for libs mpg123, a52, dts
    struct mpstr_tag   mp;                 // interface struct
    a52_state_t        *State;             // decoder state structure AC3
    dts_state_t        *Statd;             // decoder state structure DTS

// static variables for mpasource
    unsigned char inbuf[INBUFSIZE];        // input buffer for mpa-file-data
    char          outbuf[2*OUTBUFSIZE];    // output buffer for pcm samples (*2 for float)
    int           last_byte;               // last used outbuf byte
    int           size;                    // size of decoded stream in bytes (pointer to decode-buffer). Static between GetAudio calls

    double        max_volume;              //(sh0) for normalizing
    float         gain;                    // sh0: Gain that is applied to file
    bool          normalize;

    struct        scantag {                //file-info
        FILE *    mptagfile;               //contains the sample count and the file length
        __int64   bytes;
        __int64   pcmbytes;
        int       gain_pp;                 //this is in 1/1000 units
        char      tagfilename[255];
    } cur_scantag;

// from AudioHeader.h
    // Stream
    FILE    *Stream;                        // stream handle
    char    *StreamName;                    // name of the stream
    __int64 StreamOffset;                   // offset to start of the audio data
    __int64 StreamLength;                   // total length of the stream
    __int64 HeaderSize;                     // number of bytes of audio header                  //raw

    // Audio information
    int     Samplerate;                     // bit stream information                           Info.audio_samples_per_second
    int     ChannelCount;                   // number of channels                               Info.nchannels
    __int64 SampleCount;                    // number of samples per channel in the stream      Info.num_audio_samples
    int     audiobytes;                     // bytes per sample and per channel 16bit=2,float=4 BytesPerChannelSample()  (Info.sample_type=1,2,4,8,16)    //mpa
    __int64 BlockAlign;                     // number of channels x number of bytes per sample  ChannelCount * audiobytes, BytesPerAudioSample()          //raw

    int     FrameCount;                     // number of entire frames in the stream
    int     BlocksPerFrame;                 // number of blocks in a frame                //dts
    int     SamplesPerFrame;                // number of samples per channel in a frame   //dts

    int     Flags;                          // bit stream information
    int     Bitrate;                        // bit stream information
    bool    LFE;                            // is the LFE channel present?
    int     DecFlags;                       // flags used for decoding
    float   Level;                          // level of output samples
    int     Accel;                          // decoder acceleration
    int     _drc;                           // DRC

    int     Quant;                          // sample quantization (number of bits per sample)       //lpcm
    int     InQuant;                        // sample quantization (number of bits per sample)       //lpcm
    int     InBlock;                        // ChannelCount * InQuant / 8 (input bytes per sample)   //lpcm   ? BlockAlign
    int     OutBlock;                       // ChannelCount * Quant / 8 (output bytes per sample)    //lpcm   ?

    int      map[8];                        // remap channels

    // Buffers
    unsigned char *Frame;                   // raw buffer
    int FrameIndex, FrameLength;            // index and length of the current frame
    int FrameExt;                           // Extension of frame (padd, HR, ...)  //dts

    float   *Buffer;                        // samples from the current frame in interleaved order (framebuffer)
    int      BufferSize;                    // size of the framebuffer
    __int64  BufferStart;                   // sample range of the framebuffer
    __int64  BufferEnd;                     // sample range of the framebuffer

    unsigned char *Buffec;                  // processed samples from the current frame (pointer to the real framebuffer) //lpcm
    bool           bIsBluRay;               // other lpcm order                                                           //lpcm


    // Operations
    int  Synchronizd(int& Length, int& Flags, int& Samplerate, int& Bitrate);  // Synchronize DTS
    int  Synchronize(int& Length, int& Flags, int& Samplerate, int& Bitrate);  // Synchronize AC3
    int  ReadFramd();                                                          // ReadFrame   DTS
    bool ReadFrame();                                                          // ReadFrame   AC3
    bool ReadFraml();                                                          // ReadFrame   LPCM
    bool DecodeFramd();                                                        // DecodeFrame DTS
    bool DecodeFrame();                                                        // DecodeFrame AC3
    void EmptyFrame();
// from AudioHeader

public:
    UnicSource(const char filename[], int Param1, int Channels, int SampleBits, IScriptEnvironment *env);
    ~UnicSource();
    void             __stdcall SetCacheHints(int cachehints,int frame_range) {} ;
    const VideoInfo& __stdcall GetVideoInfo();
    PVideoFrame      __stdcall GetFrame(int n, IScriptEnvironment* env);
    bool             __stdcall GetParity(int n);
    void             __stdcall GetAudio(void* buf, __int64 start, __int64  count, IScriptEnvironment* env);
};
//---------------------------------------------------------------------------------------
// 2) INITIALIZATIONS
//---------------------------------------------------------------------------------------
UnicSource::UnicSource(const char *FileName, int Param1, int Channels, int SampleBits, IScriptEnvironment *env) {
    char     *Ext;
    unsigned char aux[20] = ""; //  aux buffer to read header

    int      len;               //length of input buffer
    __int64  byte_total;        //output PCM bytes
    int      ret;
    double   cur_volume;

    int Sync;                   // synch info
    int _DecFlags;              // temporary flags
    int i, j;                   // aux
    int Downmix = 0;            // desired downmixed channels

    char szRIFF[5]    = "";
    bool IsRF64       = false;  //  the stream is a RF64 file or CAF for DataSize 64 bits
    bool IsW64        = false;  //  the stream is a w64 file
    int AudioFormat;            //  can be 1 for int or 3 for float in wav
    int IgnoreLength  = 2;      //  can be 1, 2 (default) or 4
    __int64 DataSize  = 0;      //  DataSize read, 64 bits in RF64, w64 and caf
    double SamRaFloat = 0.0;    //  Samplerate in float for .caf

    // init re-map[] channel matrix
    for (i=0; i < 8; i++) map[i] = i;

    // Store filename
    Stream = NULL;
    StreamName = new char[strlen(FileName) + 1];
    strcpy(StreamName, FileName);

    // Open file
    if (!(Stream = fopen(StreamName, "rb")))
            env->ThrowError("UnicSource: unable to open file \"%s\"", StreamName);

    _fseeki64(Stream, 0, SEEK_END);          // instead vfm for >4 GB size
    StreamLength = _ftelli64(Stream);
    _fseeki64(Stream, 0, SEEK_SET);

    // Empty information structure
    memset(&Info, 0, sizeof(VideoInfo));
    Info.sample_type              = SAMPLE_INT16;
    Info.nchannels                = Channels ? Channels : 2;
    Info.audio_samples_per_second = 44100;
    Info.num_audio_samples        = 0;

    // Check if the stream is empty
    if (StreamLength < 52)                     // Dummy audio
            env->ThrowError("UnicSource: too short file \"%s\"", StreamName);

    Ext = StreamName + strlen(FileName) - 3;
    tipo = 5;
    if ( strncmp(Ext, "mp",  2) == 0 ) tipo = 1;
    else if ( strncmp(Ext, "dts", 3) == 0 ) tipo = 2;
    else if ( strncmp(Ext, "shd", 3) == 0 ) tipo = 2;
    else if ( strncmp(Ext, "ac3", 3) == 0 ) tipo = 3;
    else if ( strncmp(Ext, "pcm", 3) == 0 ) tipo = 4;
    else if ( strncmp(Ext, "wav", 3) == 0 ) {
        fread(aux, 1, 20, Stream);
        fread(aux, 1, 20, Stream);
        if ( READ_U16_LE(aux) == 85 ) tipo = 1;              // wav with mp3
        else if ( READ_U16_LE(aux) == 8192 ) tipo = 3;       // wav with ac3
        else if ( READ_U16_LE(aux) == 1 ) {
            fread(aux, 1, 12, Stream);
            if ( READ_U16_LE(aux+4) == 8191 )
            if ( READ_U16_LE(aux+6) == 59392 )
            if ( READ_U16_LE(aux+8) == 2033 )
            if ( READ_U16_LE(aux+10) == 64735 ) tipo = 2;    // dtswav
        }
        _fseeki64(Stream, 0, SEEK_SET);
    }

    switch (tipo) {
    case 1: // mpa
        normalize = false;
        if (Param1 == 1 || Param1 == 3) normalize = true;

        Info.sample_type = SAMPLE_FLOAT;
        audiobytes = Info.BytesPerChannelSample();

        strcpy(cur_scantag.tagfilename,StreamName);
        strcat(cur_scantag.tagfilename,".d2a");

        //,...     check if filesize is different. open it for r/w
        cur_scantag.bytes = 0;
        cur_scantag.pcmbytes = 0;
        cur_scantag.gain_pp = 0;

        cur_scantag.mptagfile = fopen(cur_scantag.tagfilename, "r");
        if (!cur_scantag.mptagfile) {
                cur_scantag.bytes = StreamLength ;
        }
        else {
        // read existing tagfile. Error checking missing !!
            fscanf_s(cur_scantag.mptagfile,"%u",&cur_scantag.bytes);
            fscanf_s(cur_scantag.mptagfile,"%u",&cur_scantag.pcmbytes);
            fscanf_s(cur_scantag.mptagfile,"%u",&cur_scantag.gain_pp);        // 0 indicates that no scan was done before
            fclose(cur_scantag.mptagfile);

            if (cur_scantag.bytes != StreamLength) {    //filelen is different. create new file
                cur_scantag.bytes = StreamLength;
                cur_scantag.pcmbytes = 0;
                cur_scantag.gain_pp = 0;
            }
        };

        //Init MP-File
        //can either be read correctly (pcmbytes <>0 ) or has to be newly written

        // first call -- counting samples, calculating max_volume for normalizing, decode one buffer for properties

        InitMP3(&mp);
        max_volume = 0;

        if (normalize) mp.quick_dirty = false;        //fast seek when not normalizing
        else           mp.quick_dirty = true;

        StreamOffset = 0;                             // Skip ID3v2,3,4 tag
        fread(inbuf, 1, 10, Stream);
        if ((inbuf)[0]==73 && (inbuf)[1]==68 && (inbuf)[2]==51 && ((inbuf)[3]==4 || (inbuf)[3]==3 || (inbuf)[3]==2))
            StreamOffset = (((inbuf)[9]&0x7f)|((inbuf)[8]<<7)|((inbuf)[7]<<14)|((inbuf)[6]<<21)) + 10;
        _fseeki64(Stream, StreamOffset, SEEK_SET);

        byte_total = 0;
        while ( !feof(Stream) ) {
            len = fread(inbuf, 1, INBUFSIZE, Stream);

            ret = decodeMP3(&mp,inbuf,len,outbuf,OUTBUFSIZE,&size);
            if (ret==-1) break;

            if (ret==MP3_OK) {                               //decode first output buffer if MP3_OK
                byte_total += size;
                for (j = 0; j < size/audiobytes; j++) {  // scan for max. volume
                    cur_volume = fabs(((float*)outbuf)[j]);  // sh0
                    if (cur_volume > max_volume ) max_volume = cur_volume;
                }
            }
            // decode rest of one input
            while (ret==MP3_OK)     {
                ret = decodeMP3(&mp, NULL, 0, outbuf, OUTBUFSIZE, &size);       //no new input, only decoding existing buffer
                if (ret==-1) break;
                if (ret==MP3_OK) {      //size only defined for OK-frame. if not MP3_OK buffer is done and needs new input bytes
                    byte_total += size;
                    for (j = 0; j < size/audiobytes; j++) {                   // scan for max. volume
                        cur_volume = fabs(((float*)outbuf)[j]);
                        if (cur_volume > max_volume ) max_volume = cur_volume;
                    }
                }
            }

            if (cur_scantag.pcmbytes != 0 ) {           //this was read from tagfile, skip reading completely
                byte_total = cur_scantag.pcmbytes;
                if (cur_scantag.gain_pp != 0 ) break;   //file has been scanned for normalizing
                if (!normalize) break;                  //no scan necessary anyway
            }
        }

        // Optional: create tagfile containing filelength of Stream, bytecount of output, max volume
        if (Param1 == 2 || Param1 == 3) {
            cur_scantag.mptagfile=fopen(cur_scantag.tagfilename,"w");
            if (cur_scantag.mptagfile) {
                fprintf(cur_scantag.mptagfile,"%u\n",cur_scantag.bytes);
                fprintf(cur_scantag.mptagfile,"%u\n",byte_total);
                if ( (cur_scantag.gain_pp != 0) || (!normalize) ) {     //scan made before or no scan made - recreate old file
                    fprintf(cur_scantag.mptagfile,"%u\n",cur_scantag.gain_pp);
                } else {        //use new values
                    if (max_volume>0) {
                        cur_scantag.gain_pp = int(1000.0f / max_volume) - 1;    //use calculated gain - 0.001
                    } else {
                        cur_scantag.gain_pp = 1000;                             //empty sound file - use gain=1
                    }
                    fprintf(cur_scantag.mptagfile,"%u\n", cur_scantag.gain_pp); //no max_volume calculated
                }
                fclose(cur_scantag.mptagfile);
            }
        }

        if (!normalize) {
            gain = 1.0f;
        } else {
            gain = float(cur_scantag.gain_pp) / 1000.;
        }

        // setting videoinfo-properties
        ChannelCount = 1;
        if (mp.fr.stereo==2) ChannelCount = 2;
        BlockAlign = audiobytes * ChannelCount;
        SampleCount = byte_total / BlockAlign;     //init done, now at end-of-file
        Info.nchannels = ChannelCount;             //AS25
        Info.num_audio_samples = SampleCount;
        Info.audio_samples_per_second = freqs[mp.fr.sampling_frequency];

        if (SampleCount <= 0)
                env->ThrowError("UnicSource: \"%s\" without a valid mpeg audio fraame.", StreamName);
        // Info.num_frames = Info.FramesFromAudioSamples(Info.num_audio_samples); // ??
        break;
    case 2: // dts
        // Initialize pointers
        Statd  = 0;
        Frame  = 0;
        Buffer = 0;

        // Parameters
        _drc = 0;
        if (Param1 == 1) _drc = 1;
        if (Channels > 0) Downmix = Channels;

        // Set decoder acceleration
        Accel = 0;
        // Find first frame
        Statd       = dts_init(Accel);
        FrameLength = 2048;             // dts MAX_DETECT_ERROR, max detection error (in bytes), max padd also.
        Sync = Synchronizd(FrameLength, Flags, Samplerate, Bitrate);
        if (Sync <= 0)
            env->ThrowError("UnicSource: \"%s\" is not a valid DTS file", StreamName);

        StreamOffset = Sync - 1;          // StreamOffset = (Sync - 1) % FrameLength;
        _fseeki64(Stream, StreamOffset, SEEK_SET);

        // Initialize raw buffer
        Frame      = new unsigned char[FrameLength];
        FrameIndex = -2;

        // Get number of blocks per frame
        fread(Frame, 1, FrameLength, Stream);

        if (dts_frame(Statd, Frame, &Flags, &Level, 0))
            env->ThrowError("UnicSource: error in file \"%s\"", StreamName);

        BlocksPerFrame  = dts_blocks_num(Statd);
        SamplesPerFrame = BlocksPerFrame * SAMPLES_PER_BLOCK;
        dts_free(Statd);
        Statd = 0;

        FrameCount = int((StreamLength - StreamOffset) / FrameLength);
    // New code to detect padd or HD subframes
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
                   env->ThrowError("UnicSource: \"%s\" is not a valid DTS file", StreamName);
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
            default:                    // VBR file, we need read the whole file to know the core FrameCount
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
                        _fseeki64(Stream, - 8, SEEK_CUR);                                             // vbr
                    }                                                                                 // vbr
                }                                                                                     // vbr
                FrameExt = -1;  // VBR signal                                                         // vbr
            }
            break;
        default:
                env->ThrowError("UnicSource: unknow DTS format in file \"%s\"", StreamName);
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
            env->ThrowError("UnicSource: unsupported channel configuration in file \"%s\"", StreamName);
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
                env->ThrowError("UnicSource: invalid number of channels requested");
            }

            if (Downmix < ChannelCount) {
                ChannelCount = Downmix;
                DecFlags     = _DecFlags;
            }
        }

        Info.nchannels = ChannelCount;

        // Remapping Matrix
        Channels = DecFlags & 15;
        if (LFE) Channels += 16;

        switch (Channels) {                                    //amod + 16*LFE
        // dts output order is: front center, front left, front right, rear left, rear right, LFE
        // case 0:   // 0                1 channel                              ok
        // case 2:   // 0, 1             2 chan: left, right                    ok
        // case 6:   // 0, 1, 2          2/1 chan: FLeft, FRight, RCenter       ok
        // case 8:   // 0, 1, 2, 3       2/2 chan: FLeft, FRight, RLeft, RRight ok
        // case 16:  // 0, 1             2 chan: 1 mono + LFE ok                ok
        // case 18:  // 0, 1, 2          2/0.1 chan: left, right + LFE          ok
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
        break;
    case 3: // ac3
        // Initialize pointers
        State  = 0;
        Frame  = 0;
        Buffer = 0;

        // Parameters
        _drc = 0;
        if (Param1 == 1) _drc = 1;

        if (Channels > 0) Downmix = Channels;

        // Find first frame
        FrameLength = 1048576;                // MAX_DETECT_ERROR for ac3, max detection error (1 MB), 13 sec 640 Kb/s, 21 for 192.
        Sync        = Synchronize(FrameLength, Flags, Samplerate, Bitrate);
        if (Sync <= 0)
                env->ThrowError("UnicSource: \"%s\" without a valid AC-3 header in first 1MB", StreamName);

        StreamOffset = (Sync - 1) % FrameLength;
        _fseeki64(Stream, StreamOffset, SEEK_SET);

        // Set sample type
        Info.sample_type = SAMPLE_FLOAT;

        // Get channel count
        DecFlags = Flags;                          // flags = acmod + 8*dsurmod + 16*lfeon
        LFE      = ((Flags & A52_LFE) == A52_LFE); // WAS Flags && A52_LFE !!!!!
        Channels = Flags & A52_CHANNEL_MASK;

        switch (Channels) {
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
            env->ThrowError("UnicSource: unsupported channel configuration in file \"%s\"", StreamName);
        }
        if (LFE) ChannelCount++;

        if (Downmix) {                  // Not recommended use this internal downmix here, but for compatibility ...
            switch (Downmix) {
            case 1:                     // 1 channel
                _DecFlags = A52_MONO;
                break;
            case 2:                     // 2 channels (Dolby Surround)
                _DecFlags = A52_DOLBY;
                break;
            case 4:                     // 4 channels
                _DecFlags = A52_2F2R;
                break;
            case 6:                     // 6 channels
                _DecFlags = A52_3F2R;
                if (LFE) _DecFlags |= A52_LFE;
                break;
            default:
                env->ThrowError("UnicSource: invalid number of channels requested");
            }

            if (Downmix < ChannelCount){
                ChannelCount = Downmix;
                DecFlags     = _DecFlags;
            }
        }
        Info.nchannels = ChannelCount;

        // Remapping Matrix
        switch (DecFlags & 23) {                                    //acmod + 16*LFE
        // case 1:   // 0                1 channel                              ok
        // case 0:   // 0, 1             2 chan: Channel1, Channel2             ok
        // case 2:   // 0, 1             2 chan: left, right                    ok
        // case 4:   // 0, 1, 2          2/1 chan: FLeft, FRight, RCenter       ok
        // case 6:   // 0, 1, 2, 3       2/2 chan: FLeft, FRight, RLeft, RRight ok
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
        if (Samplerate == 44100)
                FrameCount = int(((147.0 * double(StreamLength - StreamOffset)) / ( 640.0 * double(Bitrate/1000)) )+0.5);

        // Get number of samples
        SamplesPerFrame        = (BLOCKS_PER_FRAME * SAMPLES_PER_BLOCK); // number of samples per channel in a frame
        SampleCount            = __int64(FrameCount) * SamplesPerFrame;
        Info.num_audio_samples = SampleCount;

        // Initialize buffers
        Frame       = new unsigned char[FrameLength*2];         // Nick: *2 to be on the safe side ;)
        FrameIndex  = -2;

        BufferSize  = Info.nchannels * SamplesPerFrame;
        Buffer      = new float[BufferSize];
        BufferStart = BufferEnd = -1;

        // Set decoder acceleration
        Accel = 0;
        break;
    case 4: // lpcm
        // Initialize pointers
        Frame                   = 0;
        Buffec                  = 0;
        bIsBluRay               = false;

        // Parameters
        Samplerate = 0;
        if (Param1 > 0) Samplerate = Param1;

        // Check
        if ((Channels < 1) || (Channels > 8))
            env->ThrowError("UnicSource: unsupported number of channels defined");

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
            if (ChannelCount > 6)   map[4] = 7 * Quant;       // SL or BC
            if (ChannelCount > 7)   map[5] = 8 * Quant;       // SR
        }

        // Set sample type
        InQuant = SampleBits;
        switch (InQuant) {
        case 16:                // 16 bit
            Quant = InQuant;
            Info.sample_type = SAMPLE_INT16;
            break;
        case 20:                // 20 bit, verify ChannelCount even (if mono InBlock = 1 * 20 / 8 = 2.5 ??)
            if ( bIsBluRay || ( ChannelCount != 2 * int(ChannelCount / 2)))
                env->ThrowError("UnicSource: 20 bit unsupported in this context.");
        case 24:                // 24 bit
            Quant = 24;
            Info.sample_type = SAMPLE_INT24;
            break;
        case 32:                // 32 bit
            Quant = 32;
            Info.sample_type = SAMPLE_INT32;
            break;
        default:
            env->ThrowError("UnicSource: unsupported sample precision defined");
        }

        Info.nchannels = ChannelCount;                          // Set channel info
        Info.audio_samples_per_second = Samplerate;             // Set info sampling rate

	InBlock = (ChannelCount * InQuant) / 8;
	SampleCount = StreamLength / InBlock;                   // Truncate! (if incomplete last sample) ok
	Info.num_audio_samples = SampleCount;

	// Calculate number of samples per channel per frame
	SamplesPerFrame = FRAME_LENGTH / InBlock;               // always exact with 50400

	// Initialize buffers
	Frame = new unsigned char[FRAME_LENGTH];
	FrameIndex = -2;

	// We need auxiliary buffer because we cannot copy the samples directly from the raw buffer
        OutBlock = (ChannelCount * Quant) / 8;                  // output bytes per sample, only different
	Buffec = new unsigned char[SamplesPerFrame * OutBlock]; // from InBlock if 20 -> 24 bits
	BufferStart = BufferEnd = -1;
        break;
    case 5: // rawav
        HeaderSize            = 0;             //  New variable must be global see AudioHeader.h
        // Parameters
        Samplerate = 0;
        if (Param1 > 0) Samplerate = Param1;

        if (Samplerate > 0) IgnoreLength = Samplerate;  // raw data with Samplerate 1..4 is not allowed

        if ( IgnoreLength < 5 ) {                // Is not raw data, then the parameter is IgnoreLength, SampleBits and Channels are ignored.
            Samplerate = 0;                      // To allow read Samplerate 1..4

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
                            Samplerate  = READ_U32_LE(aux+4);
                            SampleBits  = READ_U16_LE(aux+14);

              // WAVE_FORMAT_EXTENSIBLE
                            if ( AudioFormat == 65534 ) {        // cbSize(0-1), ValidBitsPerSample(2-3)
                                fread(aux, 1, 10, Stream);       // ChannelMask(4-7), AudioFormat(8-9), rest of GUID not used
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
                    if (Samplerate < 1)
                        env->ThrowError("UnicSource: Don't found subchunk 'fmt '");

                    if ( strncmp(szRIFF, "data", 4) != 0 )
                        env->ThrowError("UnicSource: Don't found subchunk 'data'");

                    if ( (AudioFormat == 3) && (SampleBits == 32) )   // 3 for float
                        SampleBits = 33;                              // after recover the 32 bits and SAMPLE_FLOAT type
                    else if ( AudioFormat != 1 )                      // 1 for int
                        env->ThrowError("UnicSource: unsupported Audio Format");

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

                } else env->ThrowError("UnicSource: Don't found format 'WAVE'");

    // AU ==============================================================================================
            } else if ( strncmp(szRIFF, ".snd", 4) == 0 ) {         // .au
                fread(aux, 1, 20, Stream);
                HeaderSize = READ_U32_BE(aux);
                DataSize   = READ_U32_BE(aux+4);
                SampleBits = READ_U32_BE(aux+8);
                Samplerate = READ_U32_BE(aux+12);
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
                    env->ThrowError("UnicSource: unsupported sample precision or u-law");
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
                            Channels    = READ_U16_BE(aux);          //  Samplerate(8-17) real extended
                            SampleBits  = READ_U16_BE(aux+6);
                            AudioFormat = aux[9];                    // aux to convert real extended
                            Samplerate  = READ_U16_BE(aux+10);
                            if (AudioFormat > 14) Samplerate <<= (AudioFormat - 14);  // hack for extended real (10 bytes) valid for Samplerate range.
                            else if (AudioFormat < 14) Samplerate >>= (14 - AudioFormat);
                            if ( (SampleBits != 8) &&  (SampleBits != 16) &&  (SampleBits != 24) &&  (SampleBits != 32) )
                                env->ThrowError("UnicSource: unsupported sample precision");
                        }
                    }
        // Checks
                    if (Samplerate < 1)
                        env->ThrowError("UnicSource: Don't found subchunk 'COMM'");

                    if ( strncmp(szRIFF, "SSND", 4) != 0 )
                        env->ThrowError("UnicSource: Don't found subchunk 'SSND'");
        // Values
                    DataSize   = READ_U32_BE(aux);
                    HeaderSize += 16;
                } else
                    env->ThrowError("UnicSource: Don't found format 'AIFF'");
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
                        Samplerate  = READ_U16_BE(aux+2);
                        Samplerate  >>= 5;
                        Samplerate  += 32768 + 2048 * (AudioFormat % 16);         // mantisa
                        AudioFormat >>= 4;                                        // exponent
                        if (AudioFormat > 14) Samplerate <<= (AudioFormat - 14);  // hack for real (8 bytes) valid for Samplerate range.
                        else if (AudioFormat < 14) Samplerate >>= (14 - AudioFormat);

                        fread(szRIFF, 1, 4, Stream);                              // lpcm
                        szRIFF[4] = 0;
                        if ( strncmp(szRIFF, "lpcm", 4) != 0 )
                            env->ThrowError("UnicSource: Unssuported CAF format %s", szRIFF);
                        fread(aux, 1, 20, Stream);  // FormatFlags(0-3), BytesPerPacket(4-7), FramesPerPacket(8-11)
                        AudioFormat = aux[3];                 // 0 int, 1 float , > 1 LittleEndian
                        Channels   = READ_U16_BE(aux+14);     // ChannelsPerFrame(12-15)
                        SampleBits = READ_U16_BE(aux+18);     // BitsPerChannel(16-19)

                        if ( (AudioFormat == 1) && (SampleBits == 32) ) // float
                            SampleBits = 33;                        // after recover the 32 bits and SAMPLE_FLOAT type
                        else if ( AudioFormat != 0 )                    // 0 for int
                            env->ThrowError("UnicSource: unsupported Audio Format");
                    }
                }
        // Checks
                if (Samplerate < 1)
                    env->ThrowError("UnicSource: Don't found subchunk 'desc'");

                if ( strncmp(szRIFF, "data", 4) != 0 )
                    env->ThrowError("UnicSource: Don't found subchunk 'data'");
        // Values
                DataSize   = READ_U64_BE(aux);
                IsRF64 = true;                                 // Only for say DataSize is 64 bits
                HeaderSize += 12;

    // End formats =====================================================================================
            } else env->ThrowError("UnicSource: Unssuported header format %s", szRIFF);

            _fseeki64(Stream, HeaderSize, SEEK_SET);         // generic fseek with HeaderSize
            StreamLength -= HeaderSize;                      // Audio Data Length until eof

        // IgnoreLength
            if ( StreamLength > DataSize ) {                 // Only this have sense
                switch (IgnoreLength) {
                case 1:                                      // Force Ignore Length even for w64, RF64 and CAF
                    break;                                   // Audio Data Length until eof
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
            env->ThrowError("UnicSource: unsupported number of channels defined");

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
            env->ThrowError("UnicSource: unsupported sample precision");
        }

        Info.nchannels = Channels;                      // Set channels
        Info.audio_samples_per_second = Samplerate;     // Set sampling rate

        BlockAlign  = Channels * SampleBits / 8;        // Calculate BlockAlign needed in others routines instead Channels and SampleBits
        SampleCount = StreamLength / BlockAlign;        // Calculate and set the number of audio samples

        Info.num_audio_samples = SampleCount;
        break;
    default:
        env->ThrowError("UnicSource: unsupported audio input in file \"%s\"", StreamName);
    }
}
//---------------------------------------------------------------------------------------
// 3) DESTRUCTOR
//---------------------------------------------------------------------------------------
UnicSource::~UnicSource() {
    if (Stream) fclose(Stream);
    if (StreamName) delete[] StreamName;
    if (Frame)  delete[] Frame;
    if (Buffec) delete[] Buffec;
    if (Buffer) delete[] Buffer;
    switch (tipo) {
    case 1: // mpa
        ExitMP3(&mp);
        break;
    case 2: // dts
        if (Statd)  dts_free(Statd);
        break;
    case 3: // ac3
        if (State)  a52_free(State);
        break;
    case 4: // lpcm
        break;
    case 5: // rawav
        break;
    }
}

//---------------------------------------------------------------------------------------
// 4) MAIN PROC GetAudio
//---------------------------------------------------------------------------------------
void __stdcall UnicSource::GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env) {

    int     sample_offset;      //loop var
    int     len;                //length of input buffer
    int     ret;                //return value
    int     j;                  // aux

    __int64 i;                  // cycle counter
    __int64 Left;               // number of samples to be copied from the buffer
    int     _FrameIndex;        // index of the previous frame
    int     _FrameExt;          // aux for length of VBR HD subframes  // vbr
    int     ReadOK;             // result of ReadFramd
    float   *Input, *Output;    // buffer pointers

    unsigned char *InpuC, *OutpuC;  // buffer pointers

    //Code to prohibit illegal audio requests. Init buffer in that case
    if ( start >= Info.num_audio_samples) {             // No scan over max position.
        memset((char*)buf, 0, count * BlockAlign);      //init buffer
        return;
    }
    if ( (start + count)>= Info.num_audio_samples ) {   // No scan over max position.
        memset((char*)buf, 0, count * BlockAlign);      //init buffer
        count = Info.num_audio_samples - start;
    }
    if ( start + count <=0 ) {                          // No requests before 0 possible.
        memset((char*)buf, 0, count * BlockAlign);      //init buffer
        return;
    }
    if ( start < 0 ) {      //start filling the buffer later as requested. e.g. if start=-1 then omit 1 sample.
        memset((char*)buf, 0, count * BlockAlign);      //init buffer
        count = count + start;                          //request fewer samples
        buf = (char*)buf - start * BlockAlign;          //move buffer
        start = 0;
    }
    switch (tipo) {
    case 1: // mpa
        //start of decoding
        if (start < SampleCount) {       // go to start if trying to seek backward. SampleCount is initialized in Init()
            InitMP3(&mp);   //clear mp structure. mp.quick_dirty is set to false here
            _fseeki64(Stream, StreamOffset, SEEK_SET);
            // rewind(Stream);
            do {            // read and decode first input buffer. can be necessary more than once to fill output buffer
                len = fread(inbuf, 1, INBUFSIZE, Stream);
                ret = decodeMP3(&mp, inbuf, len, outbuf, OUTBUFSIZE, &size);
            } while (ret==MP3_NEED_MORE);
            SampleCount = 0;
            last_byte = 0;
        }
        for (sample_offset=0;sample_offset<count;sample_offset++) { //loop to fill the AviSynth request buffer

            if (SampleCount - (start+sample_offset)> 32768)         //fast searching on/off "many" samples before requested position
                 mp.quick_dirty = true;
            else mp.quick_dirty = false;

            while ( SampleCount < (start+sample_offset)) {          //seeking to requested position

                SampleCount++;
                last_byte = last_byte + ChannelCount * audiobytes;  //advance one sample in outbuf

                if (last_byte > (size-1)) {                         //request is not in outbuf >> decode more input
                    last_byte = 0;
                    ret = decodeMP3(&mp,NULL,0,outbuf,OUTBUFSIZE,&size);
                    if (ret==MP3_NEED_MORE) {                      //more decodes necessary
                        do {
                            len = fread(inbuf, 1, INBUFSIZE, Stream);
                            ret = decodeMP3(&mp,inbuf,len,outbuf,OUTBUFSIZE,&size);
                        } while (ret==MP3_NEED_MORE);
                    }
                    if (ret!=MP3_OK) return;                       //some error occured
                }
            }
            //copy buffers
            for (j = 0; j < ChannelCount; j++) {
                    ((float*)buf)[sample_offset*ChannelCount+j] = ((float*)outbuf)[last_byte/audiobytes+j] * gain;
            }
        }
        break;
    case 2: // dts
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
               if (Statd) dts_free(Statd);
               Statd = dts_init(Accel);

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
            ReadOK = ReadFramd();
            if (ReadOK == 0)
                env->ThrowError("UnicSource: error 1 in file \"%s\"", StreamName);
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
                ReadOK = ReadFramd();
                if (ReadOK == 0)
                    env->ThrowError("UnicSource: error 2 in file \"%s\"", StreamName);
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
        break;
    case 3: // ac3
        // Check if we must read a frame
        if (start < BufferStart || start > BufferEnd) {
            _FrameIndex = FrameIndex;                      // Calculate the index and the sample range of the required frame
            FrameIndex  = int(start / SamplesPerFrame);
            BufferStart = __int64(FrameIndex) * SamplesPerFrame;
            BufferEnd   = BufferStart + SamplesPerFrame;

            if (FrameIndex != _FrameIndex + 1) {           // Seek to the frame, if required

                if (State) a52_free(State);                // Reset decoder
                State = a52_init(Accel);

                if (FrameIndex == 0)
                        _fseeki64(Stream, StreamOffset, SEEK_SET);
                else {
                        _fseeki64(Stream, StreamOffset + __int64(FrameIndex - 1) * __int64(FrameLength), SEEK_SET);

                        // Read previous frame to initialize the desired frame
                        if (!ReadFrame())
                                env->ThrowError("UnicSource: error in file \"%s\"", StreamName);
                }


            }

            if (!ReadFrame())                              // Read the frame
                env->ThrowError("UnicSource: error in file \"%s\"", StreamName);
        }

        // Start decoding
        Input  = Buffer + int(start - BufferStart) * ChannelCount;
        Output = (float *)buf;
        Left   = SamplesPerFrame - (start - BufferStart);

        while (count) {
            if (count < Left) Left = count;                // Copy samples
            count -= Left;
            for (i = 0; i < Left * ChannelCount; i++) *Output++ = *Input++;

            if (count) {                                   // Check if we must read a frame
                FrameIndex++;
                if (!ReadFrame())                          // Read the frame
                    env->ThrowError("UnicSource: error in file \"%s\"", StreamName);

                Input = Buffer;                            // Reset input buffer pointer
                Left  = SamplesPerFrame;
            }
        }

        // Calculate the sample range of the current frame
        BufferStart = __int64(FrameIndex) * SamplesPerFrame;
        BufferEnd   = BufferStart + SamplesPerFrame;
        break;
    case 4: // lpcm
        // Check if we must read a frame
        if (start < BufferStart || start > BufferEnd) { // Calculate the index and the sample range of the required frame
            _FrameIndex = FrameIndex;
            FrameIndex  = int(start / (FRAME_LENGTH / InBlock));
            BufferStart = __int64(FrameIndex) * (FRAME_LENGTH / InBlock);

            if (FrameIndex != _FrameIndex + 1)          // Seek to the frame, if required
                _fseeki64(Stream, __int64(FrameIndex) * FRAME_LENGTH, SEEK_SET);

            if (!ReadFraml())                           // Read the frame
                env->ThrowError("UnicSource: error in file \"%s\"", StreamName);
        }

        InpuC = Buffec + int(start - BufferStart) * OutBlock;           // Start decoding
        OutpuC = (unsigned char *)buf;
        Left = SamplesPerFrame - (start - BufferStart);

        while (count) {
            if (count < Left) Left = count;                             // Copy samples
            memcpy(OutpuC, InpuC, unsigned int(Left * OutBlock));       // Here pending remapping channels for BluRay
            OutpuC += (Left * OutBlock);                                // BUG solved

            count -= Left;
            if (count) {                                                // Check if we must read a frame
                FrameIndex++;                                           // Read the frame
                if (!ReadFraml())
                    env->ThrowError("UnicSource: error in file \"%s\"", StreamName);

                InpuC = Buffec;                                         // Reset input buffer pointer
                Left  = SamplesPerFrame;
            }
        }

        // Calculate the sample range of the current frame
        BufferStart = __int64(FrameIndex) * (FRAME_LENGTH / InBlock);
        BufferEnd   = BufferStart         + SamplesPerFrame;
        break;
    case 5: // rawav
        _fseeki64(Stream, start * BlockAlign + HeaderSize, SEEK_SET);
        fread( buf, 1, count * BlockAlign, Stream);
        break;
    default:
        env->ThrowError("UnicSource: unsupported audio input in file \"%s\"", StreamName);
    }
}

//---------------------------------------------------------------------------------------
// 5) OPERATIONS Synchronize, ReadFrame, DecodeFrame, EmptyFrame
//---------------------------------------------------------------------------------------

// Synchronize with the audio stream: DTS
int UnicSource::Synchronizd(int& Length, int& Flags, int& Samplerate, int& Bitrate) {
    int Delta = 0;                                  // number of bytes out of synch
    int _Length;                                    // backup of Length
    unsigned char Buffer[FRAME_HEADER_LED];         // buffer for a frameheader

    // Backup Length
    _Length = Length;

    // Try synchronization
    while (Delta < _Length - 1) {
        // Read header
        if (fread(Buffer, 1, FRAME_HEADER_LED, Stream) != FRAME_HEADER_LED)
            return -1;

        // Get information
        Length = dts_syncinfo(Statd, Buffer, &Flags, &Samplerate, &Bitrate, &Length);

        if (Length) {                        // Success
            _fseeki64(Stream, -FRAME_HEADER_LED, SEEK_CUR);
            return (Delta + 1);
        } else {                             // Try again
            _fseeki64(Stream, 1 - FRAME_HEADER_LED, SEEK_CUR);
            Delta++;
        }
    }
    // Failure
    Length = _Length;
    return 0;          // Without a header in Length bytes
}

// Reads the next DTS frame from the stream and decodes it into the framebuffer
int UnicSource::ReadFramd() {
    int Sync;                               // synch info
    int Length = 0;                         // length of the current frame
    int _Flags, _Samplerate, _Bitrate;      // bit stream information

    while (!Length) {                       // Read next frame from stream

       if (fread(Frame, 1, FrameLength, Stream) != FrameLength) { // Error at the end of the stream, mute frame
           EmptyFrame();
           return -1;                       // When extract core we need break
       }

       // Get frame information
       Length = dts_syncinfo(Statd, Frame, &_Flags, &_Samplerate, &_Bitrate, &Length);

       // Check if we need synchronization
       if (!Length) {

           _fseeki64(Stream, 1 - FrameLength, SEEK_CUR);          // Seek back in the stream
           Length = FrameLength;                                  // Try synchronization
           Sync   = Synchronizd(Length, _Flags, _Samplerate, _Bitrate);

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
    if (!DecodeFramd()) EmptyFrame();

    // new code to skip padd and HD subframes
    if (FrameExt < 0) {                                                                                 // vbr
        if (9 == fread(Frame, 1, 9, Stream))  {                                                         // vbr
            if ((((Frame)[3]<<24)|((Frame)[2]<<16)|((Frame)[1]<<8)|((Frame)[0]&0xff)) == 622876772) {   // vbr
                 Length = 1 + ((((Frame)[6]&0x0f)<<11)|((Frame)[7]<<3)|((Frame)[8]>>5));                // vbr
                 _fseeki64(Stream, Length - 9, SEEK_CUR);                                               // vbr
            } else                                                                                      // vbr
                _fseeki64(Stream, - 9, SEEK_CUR);                                                       // vbr
        }                                                                                               // vbr
    }                                                                                                   // vbr
    if (FrameExt > 0) _fseeki64(Stream, FrameExt, SEEK_CUR);                                            // pad
    return 1;
}

// Decode DTS frame from the raw buffer
bool UnicSource::DecodeFramd() {
    int    blk, i, j;               // cycle counters
    float *Input, *Output;          // buffer pointers

    // Start decoding the frame
    if (dts_frame(Statd, Frame, &DecFlags, &Level, 0) || (dts_blocks_num(Statd) != BlocksPerFrame))
        return false;

    // Dynamic range compression
    if(0==_drc) dts_dynrng(Statd, 0, 0);

    // The decoded samples will be in the following sequencial order:
    //   front center, front left, front right, rear left, rear right, LFE
    // If one of the channels is not present, it is skipped and the following
    // channels are shifted accordingly. We have to put the channels in
    // the proper interleaved order, based on the requested channel config.

    Output = Buffer;  // Decode each block
    for (blk = 0; blk < BlocksPerFrame; blk++) {

        if (dts_block(Statd)) return false; // Decode block
        Input = dts_samples(Statd);         // Copy channel

        for (j = 0; j < ChannelCount; j++) {
            for (i = 0; i < SAMPLES_PER_BLOCK; i++) {
                Output[i * ChannelCount + map[j]] = *Input++;     // mapping matrix

            }
        }
        Output += SAMPLES_PER_BLOCK * ChannelCount;               // Next block
    }

    return true;
}

// Empty the framebuffer
void UnicSource::EmptyFrame() {
        for (int i = 0; i < SamplesPerFrame * ChannelCount; i++) Buffer[i] = 0;
}

// Synchronize with the audio stream: AC3
int UnicSource::Synchronize(int& Length, int& Flags, int& Samplerate, int& Bitrate) {
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
        Length = a52_syncinfo(Buffer, &Flags, &Samplerate, &Bitrate);

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

bool UnicSource::ReadFrame() {              // AC3
    int Sync;                               // synch info
    int Length;                             // length of the current frame
    int _Flags, _Samplerate, _Bitrate;      // bit stream information

    // Read next frame from stream
    if (fread(Frame, 1, FrameLength, Stream) != FrameLength) {
        EmptyFrame();                       // Error at the end of the stream, mute frame
        return true;
    }

    Length = a52_syncinfo(Frame, &_Flags, &_Samplerate, &_Bitrate);    // Get frame information

    if (!Length) {                                      // Check if we need synchronization
        _fseeki64(Stream, 1 - FrameLength, SEEK_CUR);   // Seek back in the stream

        Length = FrameLength;                           // Try synchronization
        Sync   = Synchronize(Length, _Flags, _Samplerate, _Bitrate);

        if (!Sync || Sync < 0) {                        // Serious damage in the stream, mute frame
            EmptyFrame();                               // Mute frame
            return true;
        }

        // Check if the frame has proper length and BSI
        if ( (Length != FrameLength && Samplerate != 44100) || ( (_Flags & 23)  != (Flags & 23)) || (_Samplerate != Samplerate)) {
            EmptyFrame();                               // Could just be a broken frame. Blank out and continue :D
            return true;
        }
        else if ( (Length != FrameLength && Samplerate == 44100) ) {
        // Handle 44100khz's weird two different FrameLengths. Just make sure it's not a ludicrous difference (difference should only be 1 )
            if ( abs(Length-FrameLength) > 2 ) return false;
            FrameLength = Length;
        }

        if (fread(Frame, 1, FrameLength, Stream) != FrameLength) {  // Read frame
            EmptyFrame();           // Error at the end of the stream, mute frame
            return true;
        }
    } else {                        // Check if the frame has proper length and BSI
        if ( (Length != FrameLength && Samplerate != 44100) || ((_Flags & 23) != (Flags & 23)) || (_Samplerate != Samplerate)) {
            EmptyFrame();           // Could just be a broken frame. Blank out and continue :D
            return true;
        }
        else if ( (Length != FrameLength && Samplerate == 44100) ) {
        // Handle 44100khz's weird two different FrameLengths. Just make sure it's not a ludicrous difference (difference should only be 1 )
            if ( abs(Length-FrameLength) > 2 ) return false;

            _fseeki64(Stream, -FrameLength, SEEK_CUR);         // Re-do all this with new FrameLength
            FrameLength = Length;
            return ReadFrame();                                // Recursion!
        }
    }

    if (!DecodeFrame()) EmptyFrame();                          // Decode frame
    return true;
}

// Decode AC3 frame from the raw buffer
bool UnicSource::DecodeFrame() {
    int    blk, i, j;       // cycle counters
    float *Input, *Output;  // buffer pointers

    // Start decoding the frame
    if (a52_frame(State, Frame, &DecFlags, &Level, 0)) return false;

    if(0==_drc) a52_dynrng(State, 0, 0);             // Dynamic range compression !! dimzon !!

    // The decoded samples will be in the following sequencial order:
    //   LFE, front left, front center, front right, rear left, rear right
    // If one of the channels is not present, it is skipped and the following
    // channels are shifted accordingly. We have to put the channels in
    // the proper interleaved order, based on the requested channel config.
    Output = Buffer;                                 // Decode each block
    for (blk = 0; blk < BLOCKS_PER_FRAME; blk++) {

        if (a52_block(State)) return false;          // Decode block
        Input = a52_samples(State);                  // Copy channel

        for (j = 0; j < ChannelCount; j++)
            for (i = 0; i < SAMPLES_PER_BLOCK; i++)
                Output[i * ChannelCount + map[j]] = *Input++;  // mapping matrix

        Output += SAMPLES_PER_BLOCK * ChannelCount;            // Next block
    }
    return true;
}

// Reads the next LPCM frame from the stream and copies the samples into the buffer
bool UnicSource::ReadFraml() {
    int            i, j, k, byt;         // counters
    unsigned char *Input, *Output;  // input/output buffer pointers

    if ((FrameLength = fread(Frame, 1, FRAME_LENGTH, Stream)) < 0)  // Read next frame from stream
            return false;                                           // Read error

    // Calculate number of samples per channel per frame
    SamplesPerFrame = FrameLength / InBlock;

    // Convert samples
    Input = Frame;
    Output = Buffec;

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
       switch (InQuant) {
       case 16:                                 // 16 bit, Swap bytes
           for (i = 0; i < FrameLength; i += 2) {
               Output[i]     = Input[i + 1];
               Output[i + 1] = Input[i];
           }
           break;
       case 20:                                 // 20 bit, Convert to 24 bit
           for (i = 0; i < FrameLength / (5 * ChannelCount); i++) {
               for (j = 0; j < ChannelCount; j++) {
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
       case 24:                                // 24 bit
           for (i = 0; i < FrameLength / (6 * ChannelCount); i++) {
               for (j = 0; j < 2 * ChannelCount; j++) {             // 4 channel              // 2 channel  // 1 channel
                   Output[0] = Input[4 * ChannelCount + j];         //16,17,18,19,20,21,22,23 // 8, 9,10,11 // 4, 5 // 4c, 4c+1, 4c+2, ..., 6c-1
                   Output[1] = Input[2 * j + 1];                    // 1, 3, 5, 7, 9,11,13,15 // 1, 3, 5, 7 // 1, 3 // 1, 3, 5, ..., 4c-1
                   Output[2] = Input[2 * j];                        // 0, 2, 4, 6, 8,10,12,14 // 0, 2, 4, 6 // 0, 2 // 0, 2, 4, ..., 4c-2
                   Output += 3;
               }
               Input += 6 * ChannelCount;
           }
           break;
       case 32:                                 // 32 bit, Intuit a hypothetical 32bit LPCM packing
           for (i = 0; i < FrameLength / (8 * ChannelCount); i++) {
               for (j = 0; j < 2 * ChannelCount; j++) {              // 2 channel  // 1 channel
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
           return false;        // Oops!
       }
    }
    // Success
    return true;
}

//---------------------------------------------------------------------------------------
// 6) AUX PROCS GetFrame, GetParity, GetVideoInfo
//---------------------------------------------------------------------------------------
PVideoFrame       __stdcall UnicSource::GetFrame(int n, IScriptEnvironment* env) { return NULL; }
bool              __stdcall UnicSource::GetParity(int n)                         { return false; }
const VideoInfo & __stdcall UnicSource::GetVideoInfo()                           { return Info; }

/*=============================================================================
   7)     Implementation
=============================================================================*/
AVSValue __cdecl Create_UnicSource(AVSValue args, void* user_data, IScriptEnvironment* env) {
        return new UnicSource(args[0].AsString(""), args[1].AsInt(0), args[2].AsInt(0), args[3].AsInt(0), env);
}

/*-----------------------------------------------------------------------------
   8)     AviSynth plugin initialization
-----------------------------------------------------------------------------*/
extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env) {
        env->AddFunction("UnicAudioSource", "s[param1]i[channels]i[samplebits]i", Create_UnicSource, 0);
        return "Audio Decoder Plugin";
}
