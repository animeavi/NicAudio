#pragma warning(disable:4244)
#pragma warning(disable:4996)
/*
** MPASource. version 0.3
**   tagfile creation added. float output added (thanks to sh0dan)
** (c) 2002 - Ernst Peché
** AviSynth Plugin for AviSynth 2.0x based on mpg123, version from lame
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

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>
//#include "vfm.h"
#include "avisynth.h"
#include "AudioHeader.h"

#include "mpg123\interface.h"

#define INBUFSIZE 16384
#define OUTBUFSIZE 8192

class MPASource : public IClip {
        VideoInfo vi;

// static variables for mpg123
        FILE* mpfile;           //mpa-file
        struct mpstr_tag mp;    //interface struct
        __int64  StreamOffset;  // offset to start of the audio data

// static variables for mpasource
        unsigned char inbuf[INBUFSIZE]; //input buffer for mpa-file-data
        char outbuf[2*OUTBUFSIZE];      //output buffer for pcm samples (*2 for float)
        __int64 last_sample;    //last requested sample
        int last_byte;  //last used outbuf byte
        int size;       //size of decoded stream in bytes (pointer to decode-buffer). Static between GetAudio calls

        int channels;   //number of channels (1 or 2)
        int audiobytes; //bytes per sample and per channel. 16bit=2, float=4
        double max_volume;                     //(sh0) for normalizing
        float gain;  // sh0: Gain that is applied to file
        bool normalize;

        struct scantag  //file-info
        {
                FILE * mptagfile;       //contains the sample count and the file length
                __int64 bytes;
                __int64 pcmbytes;
                int gain_pp;    //this is in 1/1000 units
                char tagfilename[255];
        } cur_scantag;

//for testing
        char dbgbuf[255];

public:
    MPASource(const char filename[], const bool _normalize, IScriptEnvironment* env);
        ~MPASource();
        void __stdcall SetCacheHints(int cachehints,int frame_range) {} ;
        PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);
    bool __stdcall GetParity(int n);
    void __stdcall GetAudio(void* buf, __int64 start, __int64  count, IScriptEnvironment* env);
    const VideoInfo& __stdcall GetVideoInfo();

private:
        static __inline short Saturate(int n) {
                if (n <= -32768) return -32768;
                if (n >= 32767) return 32767;
                return (short)n;
        }
};

MPASource::MPASource(const char filename[], const bool _normalize, IScriptEnvironment* env) {
        vi.width = 8;   //generate a small video. helpful for testing
        vi.height = 8;
        vi.fps_numerator = 25;
        vi.fps_denominator = 1;
        vi.pixel_type = VideoInfo::CS_YUY2;
        vi.sample_type = SAMPLE_FLOAT;

        normalize = _normalize;

        audiobytes = vi.BytesPerChannelSample();

        strcpy(cur_scantag.tagfilename,filename);
        strcat(cur_scantag.tagfilename,".d2a");

        mpfile = fopen(filename, "rb");
        if (!mpfile) {
                env->ThrowError("File could not be opened!");
        }

//,...     check if filesize is different. open it for r/w
        cur_scantag.bytes = 0;
        cur_scantag.pcmbytes = 0;
        cur_scantag.gain_pp = 0;

        StreamOffset = 0;
        fread(inbuf, 1, 10, mpfile);
        if ((inbuf)[0]==73 && (inbuf)[1]==68 && (inbuf)[2]==51 && ((inbuf)[3]==4 || (inbuf)[3]==3 || (inbuf)[3]==2))
                    StreamOffset = (((inbuf)[9]&0x7f)|((inbuf)[8]<<7)|((inbuf)[7]<<14)|((inbuf)[6]<<21)) + 10;

        _fseeki64(mpfile, 0, SEEK_END);
        __int64 fl = _ftelli64(mpfile) ;
        _fseeki64(mpfile, StreamOffset, SEEK_SET);

        cur_scantag.mptagfile = fopen(cur_scantag.tagfilename, "r");
        if (!cur_scantag.mptagfile) {
                cur_scantag.bytes = fl ;
        }
        else {
// read existing tagfile. Error checking missing !!
                fscanf_s(cur_scantag.mptagfile,"%u",&cur_scantag.bytes);
                fscanf_s(cur_scantag.mptagfile,"%u",&cur_scantag.pcmbytes);
                fscanf_s(cur_scantag.mptagfile,"%u",&cur_scantag.gain_pp);        // 0 indicates that no scan was done before
                fclose(cur_scantag.mptagfile);

                if (cur_scantag.bytes != fl) {
//filelen is different. create new file
                        cur_scantag.bytes = fl;
                        cur_scantag.pcmbytes = 0;
                        cur_scantag.gain_pp = 0;
                }
        };

//Init MP-File
//can either be read correctly (pcmbytes <>0 ) or has to be newly written

// first call -- counting samples, calculating max_volume for normalizing, decode one buffer for properties

        int len;        //length of input buffer
        __int64 byte_total;     //output PCM bytes
        int ret;
        double cur_volume;

        InitMP3(&mp);
        max_volume = 0;

        if (normalize) {        //fast seek when not normalizing
                mp.quick_dirty = false;
        } else {
                mp.quick_dirty = true;
        }
        byte_total = 0;



        while ( !feof(mpfile) )
        {
                len = fread(inbuf, 1, INBUFSIZE, mpfile);

                ret = decodeMP3(&mp,inbuf,len,outbuf,OUTBUFSIZE,&size);
                if (ret==-1) break;

                if (ret==MP3_OK) {      //decode first output buffer if MP3_OK
                        byte_total = byte_total+size;
                        for (int j = 0; j<size/audiobytes; j++) {       // scan for max. volume
                                cur_volume = fabs(((float*)outbuf)[j]);  // sh0
                                if (cur_volume > max_volume ) max_volume = cur_volume;
                        }
                }
// decode rest of one input
                while (ret==MP3_OK)     {
                        ret = decodeMP3(&mp, NULL, 0, outbuf, OUTBUFSIZE, &size);       //no new input, only decoding existing buffer
                        if (ret==-1) break;
                        if (ret==MP3_OK) {      //size only defined for OK-frame. if not MP3_OK buffer is done and needs new input bytes
                                byte_total = byte_total+size;
                                for (int j = 0; j<size/audiobytes; j++) {       // scan for max. volume
                                        cur_volume = fabs(((float*)outbuf)[j]);
                                        if (cur_volume > max_volume ) max_volume = cur_volume;
                                }
                        }
                }

                if (cur_scantag.pcmbytes != 0 ) {       //this was read from tagfile, skip reading completely
                        byte_total = cur_scantag.pcmbytes;
                        if (cur_scantag.gain_pp != 0 ) break;   //file has been scanned for normalizing
                        if (!normalize) break;  //no scan necessary anyway
                }
#ifdef _DEBUG
                sprintf(dbgbuf, "filepos %.10d\n", mpfile.tellg());
                OutputDebugString(dbgbuf);
#endif

        }
        // setting videoinfo-properties
        vi.audio_samples_per_second = freqs[mp.fr.sampling_frequency];
        (mp.fr.stereo==2) ? channels=2 : channels = 1;

        if (mp.fr.stereo==2) {
                channels = 2;
                vi.num_audio_samples = byte_total / (audiobytes*channels);
                vi.nchannels = 2;       //AS25
        }
        else {
                channels = 1;
                vi.num_audio_samples = byte_total / (audiobytes*channels);
                vi.nchannels = 1;//AS25
        }
        last_sample = vi.num_audio_samples;     //init done, now at end-of-file
        vi.num_frames = vi.FramesFromAudioSamples(vi.num_audio_samples);


//      create tagfile containing filelength of mpfile, bytecount of output, max volume
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
                                cur_scantag.gain_pp = 1000;     //empty sound file - use gain=1
                        }
                        fprintf(cur_scantag.mptagfile,"%u\n", cur_scantag.gain_pp);//no max_volume calculated
                }
                fprintf(cur_scantag.mptagfile,"%u\n",StreamOffset);
                fclose(cur_scantag.mptagfile);
        }

        if (!normalize) {
                gain = 1.0f;
        } else {
                gain = float(cur_scantag.gain_pp) / 1000.;
        }
}

MPASource::~MPASource() {
        ExitMP3(&mp);
        fclose(mpfile);
}


void __stdcall MPASource::GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env) {
        int sample_offset;      //loop var
        int len;        //length of input buffer
        int ret;        //return value


//Code to prohibit illegal audio requests. Init buffer in that case

        if ( start >= vi.num_audio_samples) {   // No scan over max position.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                return;
        }

        if ( (start + count)>= vi.num_audio_samples ) {  // No scan over max position.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                count = vi.num_audio_samples - start;
        }

        if ( start + count <=0 ) { // No requests before 0 possible.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                return;
        }

        if ( start < 0 ) {      //start filling the buffer later as requested. e.g. if start=-1 then omit 1 sample.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                count = count + start;  //request fewer samples
                buf = (char*)buf - start * vi.BytesPerAudioSample();    //move buffer
                start = 0;
        }

//start of decoding
        if (start < last_sample)        // go to start if trying to seek backward. last_sample is initialized in Init()
        {
                InitMP3(&mp);   //clear mp structure. mp.quick_dirty is set to false here
                // rewind(mpfile);
                _fseeki64(mpfile, StreamOffset, SEEK_SET);

                do // read and decode first input buffer. can be necessary more than once to fill output buffer
                {
                        len = fread(inbuf, 1, INBUFSIZE, mpfile);

                        ret = decodeMP3(&mp, inbuf, len, outbuf, OUTBUFSIZE, &size);
                } while (ret==MP3_NEED_MORE);
                last_sample = 0;
                last_byte = 0;
        }

        for (sample_offset=0;sample_offset<count;sample_offset++){      //loop to fill the AviSynth request buffer

                if (last_sample - (start+sample_offset)> 32768) {       //fast searching on/off "many" samples before requested position
                        mp.quick_dirty = true;
                } else {
                        mp.quick_dirty = false;
                }

                while ( last_sample < (start+sample_offset))    //seeking to requested position
                {
                        last_sample++;
                        last_byte = last_byte + channels * audiobytes;  //advance one sample in outbuf

                        if (last_byte > (size-1))       {       //request is not in outbuf >> decode more input
                                last_byte = 0;
                                ret = decodeMP3(&mp,NULL,0,outbuf,OUTBUFSIZE,&size);
                                if (ret==MP3_NEED_MORE) {       //more decodes necessary
                                        do
                                        {

                                                len = fread(inbuf, 1, INBUFSIZE, mpfile);
                                                ret = decodeMP3(&mp,inbuf,len,outbuf,OUTBUFSIZE,&size);
                                        } while (ret==MP3_NEED_MORE);
                                }
                                if (ret!=MP3_OK) return;        //some error occured
                        }
                }
                //copy buffers
                for (int i = 0; i<channels; i++) {
                        ((float*)buf)[sample_offset*channels+i] = ((float*)outbuf)[last_byte/audiobytes+i] * gain;
                }
        }
}


PVideoFrame __stdcall MPASource::GetFrame(int n, IScriptEnvironment* env) {
//only for testing. Is not called when using AudioDub
    PVideoFrame dst = env->NewVideoFrame(vi);
    BYTE * yuy2p = dst->GetWritePtr();
    const int dst_pitch = dst->GetPitch();

    for (int y = 0; y < vi.height; y++) {
        for (int x = 0; x < vi.width/2; x++) {  // each x is a double pixel in YUY2
            yuy2p[x*4 + 0] = 248; //Y
            yuy2p[x*4 + 1] = 128; //U
            yuy2p[x*4 + 2] = 248; //Y
            yuy2p[x*4 + 3] = 128; //U
        }
        yuy2p += dst_pitch;
    }
    return dst;
}


class BufferAudio : public GenericVideoFilter {

        char dbgbuf[255];
        char * cache;
        int samplesize;
        int maxsamplecount;
        __int64 cache_start;
        __int64 cache_count;

public:
    BufferAudio(PClip _child, int buffer, IScriptEnvironment* env):GenericVideoFilter(_child){

//buffer is in seconds
//typical: 48000 samples * 4 bytes (float) ~ 256 kB for 1 sec per channel
                samplesize = vi.BytesPerAudioSample();
                cache = new char[buffer * samplesize * vi.SamplesPerSecond()];
                maxsamplecount = buffer * vi.SamplesPerSecond() - 1;
                cache_start=0;
                cache_count=0;

#ifdef _DEBUG
        sprintf(dbgbuf, "CA:Size %.6d\n", maxsamplecount);
        OutputDebugString(dbgbuf);
#endif
        }

        void __stdcall BufferAudio::GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env);

        ~BufferAudio() {
                delete cache;
        };
};


void __stdcall BufferAudio::GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env) {

        __int64 shiftsamples;

#ifdef _DEBUG
        sprintf(dbgbuf, "CA:Get %.6d %.6d %.6d %.6d\n", int(start), int(count), int(cache_start), int(cache_count));
        OutputDebugString(dbgbuf);
#endif

//Code to prohibit illegal audio requests. Init buffer in that case

        if ( start >= vi.num_audio_samples) {   // No scan over max position.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                return;
        }

        if ( (start + count)>= vi.num_audio_samples ) {  // No scan over max position.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                count = vi.num_audio_samples - start;
        }

        if ( start + count <=0 ) { // No requests before 0 possible.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                return;
        }

        if ( start < 0 ) {      //start filling the buffer later as requested. e.g. if start=-1 then omit 1 sample.
                memset((char*)buf, 0, count * vi.BytesPerAudioSample());        //init buffer
                count = count + start;  //request fewer samples
                buf = (char*)buf - start * vi.BytesPerAudioSample();    //move buffer
                start = 0;
        }

//end of illegal request code

        if (count>maxsamplecount) {             //is cache big enough?

#ifdef _DEBUG
                sprintf(dbgbuf, "CA:Cache too small.");
                OutputDebugString(dbgbuf);
#endif
                child->GetAudio(buf, start, count, env);

                //fill up buffer. maybe useful next call
                cache_start = start+count-maxsamplecount;
                cache_count = maxsamplecount;
                memcpy(cache, buf, vi.BytesFromAudioSamples(maxsamplecount));
                return;

                return;
        }

        if ( (start<cache_start) || (start>=cache_start+cache_count) ){ //first sample is before or behind cache -> restart cache

#ifdef _DEBUG
                sprintf(dbgbuf, "CA:Restart");
                OutputDebugString(dbgbuf);
#endif

                child->GetAudio(cache, start, count, env);
                cache_start = start;
                cache_count = count;
        } else {        //at least start sample is in cache
                if ( start + count > cache_start + cache_count ) {//cache is too short. Else all is already in the cache
                        if ((start - cache_start + count)>maxsamplecount) {     //cache shifting is necessary
                                shiftsamples = start - cache_start + count - maxsamplecount;

                                if ( (start - cache_start)/2 > shiftsamples ) { //shift only half cache if possible
                                        shiftsamples = (start - cache_start)/2;
                                }

                                memmove(cache, cache+shiftsamples*samplesize,(cache_count-shiftsamples)*samplesize);

                                cache_start = cache_start + shiftsamples;
                                cache_count = cache_count - shiftsamples;
                        }

                        //append to cache
                        child->GetAudio(cache + cache_count*samplesize, cache_start + cache_count, start+count-(cache_start+cache_count), env);
                        cache_count = cache_count + start+count-(cache_start+cache_count);
                }
        }

        //copy cache to buf
        memcpy(buf,cache + (start - cache_start)*samplesize, count*samplesize);
}


bool __stdcall MPASource::GetParity(int n) { return false; }
const VideoInfo & __stdcall MPASource::GetVideoInfo() { return vi; }

/*=============================================================================
        Implementation
=============================================================================*/

AVSValue __cdecl Create_MPASource(AVSValue args, void* user_data, IScriptEnvironment* env) {
        return new MPASource(args[0].AsString(""), args[1].AsBool(false), env);
}

AVSValue __cdecl Create_BufferAudio(AVSValue args, void* user_data, IScriptEnvironment* env) {
        return new BufferAudio(args[0].AsClip(), args[1].AsInt(1), env);
}

/*-----------------------------------------------------------------------------
        AviSynth plugin initialization
-----------------------------------------------------------------------------*/

extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env)
{
        env->AddFunction("NicMPG123Source","s[normalize]b",Create_MPASource,0);
        env->AddFunction("NicBufferAudio","c[buffer]i",Create_BufferAudio,0);
//@@@   env->AddFunction("NicRawPCMSource", "s[samplerate]i[samplebits]i[channels]i", Create_RawPcmSource, 0);
        env->AddFunction("RaWavSource", "s[samplerate]i[samplebits]i[channels]i", m2RaWavSource::Create, 0);

        env->AddFunction("NicAC3Source", "s[channels]i[drc]i", m2AudioAC3Source::Create, 0);
        env->AddFunction("NicDTSSource", "s[channels]i[drc]i", m2AudioDTSSource::Create, 0);
        env->AddFunction("NicLPCMSource", "s[samplerate]i[samplebits]i[channels]i", m2AudioLPCMSource::Create, 0);
//@@@   env->AddFunction("NicMPASource", "s[channels]i", m2AudioMPASource::Create, 0);

        return "Audio Decoder Plugin";
}
