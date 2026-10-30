/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DirectShow "WM ASF Writer" (CLSID_WMAsfWriter) replacement: encodes WMV2/WMA2 into ASF using FFmpeg. */
#define COBJMACROS
#define STRSAFE_NO_DEPRECATE
#define WIN32_LEAN_AND_MEAN
#define WMCreateSyncReader hdr_WMCreateSyncReader
#define WMCreateProfileManager hdr_WMCreateProfileManager
#include <windows.h>
#include <math.h>
#include <ole2.h>
#include <dshow.h>
#include <wmsdk.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

void logmsg(const char *fmt, ...);
const char *guidstr(const GUID *g);

static const CLSID CLSID_WMAsfWriter_ = {0x7c23220e, 0x55bb, 0x11d3, {0x8b, 0x16, 0x00, 0xc0, 0x4f, 0xb6, 0xbd, 0x3d}};
static const GUID G_MemAllocClsid = {0x1e651cc0, 0xb199, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
static const GUID G_RGB24 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID G_RGB32 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID G_Video = {0x73646976, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_Audio = {0x73647561, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_PCM = {0x00000001, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_FmtVideo = {0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID G_FmtWave = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID G_IMiscFlags = {0x2dd74950, 0xa890, 0x11d1, {0xab, 0xe8, 0x00, 0xa0, 0xc9, 0x05, 0xf3, 0x75}};
static const GUID G_IConfigAsfWriter = {0x45086030, 0xf7e4, 0x486a, {0xb5, 0x04, 0x82, 0x6b, 0xb5, 0x79, 0x2a, 0x3b}};
static const GUID G_IWMVideoMediaProps = {0x96406bcf, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_IWMMediaProps_ = {0x96406bce, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_IUnknown_ = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

#define VFW_E_NOT_CONNECTED_ ((HRESULT)0x80040209)
#define VFW_E_TYPE_NOT_ACCEPTED_ ((HRESULT)0x8004022A)
#define VFW_E_NOT_FOUND_ ((HRESULT)0x80040216)
#define VFW_E_ALREADY_CONNECTED_ ((HRESULT)0x80040204)
#define VFW_E_NOT_STOPPED_ ((HRESULT)0x80040225)
#define VFW_E_WRONG_STATE_ ((HRESULT)0x80040227)

/* custom IConfigAsfWriter interface */
typedef struct IConfigAsfWriterX IConfigAsfWriterX;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(IConfigAsfWriterX *, REFIID, void **);
    ULONG (WINAPI *AddRef)(IConfigAsfWriterX *);
    ULONG (WINAPI *Release)(IConfigAsfWriterX *);
    HRESULT (WINAPI *ConfigureFilterUsingProfileId)(IConfigAsfWriterX *, DWORD);
    HRESULT (WINAPI *GetCurrentProfileId)(IConfigAsfWriterX *, DWORD *);
    HRESULT (WINAPI *ConfigureFilterUsingProfileGuid)(IConfigAsfWriterX *, REFGUID);
    HRESULT (WINAPI *GetCurrentProfileGuid)(IConfigAsfWriterX *, GUID *);
    HRESULT (WINAPI *ConfigureFilterUsingProfile)(IConfigAsfWriterX *, IWMProfile *);
    HRESULT (WINAPI *GetCurrentProfile)(IConfigAsfWriterX *, IWMProfile **);
    HRESULT (WINAPI *SetIndexMode)(IConfigAsfWriterX *, BOOL);
    HRESULT (WINAPI *GetIndexMode)(IConfigAsfWriterX *, BOOL *);
} IConfigAsfWriterXVtbl;
struct IConfigAsfWriterX { const IConfigAsfWriterXVtbl *lpVtbl; };

#pragma pack(push, 1)
typedef struct { RECT rcSource, rcTarget; DWORD dwBitRate, dwBitErrorRate; LONGLONG AvgTimePerFrame; BITMAPINFOHEADER bmi; } VIH_;
#pragma pack(pop)

typedef struct WF WF;
typedef struct WP {
    IPin pin;
    IMemInputPin mip;
    WF *f;
    int kind; /* 1 video, 0 audio */
    WCHAR name[32];
    IPin *peer;
    int w, h, bpp, flip; LONGLONG avg;
    int ach, arate;
    int eos;
    long nrecv;
} WP;

struct WF {
    IBaseFilter bf;
    IFileSinkFilter2 fs;
    IConfigAsfWriterX cw;
    IAMFilterMiscFlags mf;
    LONG ref;
    CRITICAL_SECTION lock;
    FILTER_STATE state;
    IFilterGraph *graph; WCHAR gname[128];
    WCHAR *file; DWORD mode;
    WP pins[2]; WP *order[2]; int npins;
    /* profile */
    DWORD vbitrate, abitrate; int pw, ph; LONGLONG pavg; DWORD quality; int parate, pach;
    /* encoder state */
    AVFormatContext *oc; AVStream *vst, *ast; AVCodecContext *venc, *aenc;
    struct SwsContext *sws; struct SwrContext *swr; AVAudioFifo *fifo;
    AVFrame *vframe, *aframe; AVPacket *pkt;
    int started, finished, ew, eh; LONGLONG vlast; int64_t apts;
    long nv, na;
};
#define W_BF(p) ((WF *)(p))
#define W_FS(p) ((WF *)((BYTE *)(p) - offsetof(WF, fs)))
#define W_CW(p) ((WF *)((BYTE *)(p) - offsetof(WF, cw)))
#define W_MF(p) ((WF *)((BYTE *)(p) - offsetof(WF, mf)))
#define P_PIN(p) ((WP *)(p))
#define P_MIP(p) ((WP *)((BYTE *)(p) - offsetof(WP, mip)))

/* ---------- encoding ---------- */
static void write_packets(WF *f, AVCodecContext *enc, AVStream *st)
{
    while (avcodec_receive_packet(enc, f->pkt) >= 0) {
        av_packet_rescale_ts(f->pkt, enc->time_base, st->time_base);
        f->pkt->stream_index = st->index;
        int r = av_interleaved_write_frame(f->oc, f->pkt);
        if (r < 0) logmsg("write_frame failed %d", r);
    }
}
/* ExportFormat in TheMoviesGfx.ini (next to MoviesSE.exe): "mp4" (default; H.264 + AAC) or "wmv" (the old WMV2 + WMA2 asf) */
static int want_mp4(void)
{
    char ini[MAX_PATH], v[32]; char *sl;
    if (!GetModuleFileNameA(NULL, ini, sizeof ini)) return 1;
    sl = strrchr(ini, '\\'); if (!sl) return 1; strcpy(sl + 1, "TheMoviesGfx.ini");
    GetPrivateProfileStringA("Gfx", "ExportFormat", "mp4", v, sizeof v, ini);
    return _stricmp(v, "wmv") != 0 && _stricmp(v, "asf") != 0;
}
/* "<dir>\name.wmv" -> "<dir>\name.mp4", or "name(1).mp4" ... when that already exists (the game only looks for existing .wmv names) */
static void mp4_path(char *path)
{
    char base[1024]; char *dot; int n = 0;
    strcpy(base, path); dot = strrchr(base, '.'); if (dot && !strchr(dot, '\\')) *dot = 0;
    sprintf(path, "%s.mp4", base);
    while (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && n < 1000) sprintf(path, "%s(%d).mp4", base, ++n);
}
/* ExportCRF in TheMoviesGfx.ini: x264 constant quality, 14 (huge, near lossless) .. 30 (small); default 22 (about 4-6 Mbps at 1080p for game footage) */
static int export_crf(void)
{
    char ini[MAX_PATH]; char *sl; int v;
    if (!GetModuleFileNameA(NULL, ini, sizeof ini)) return 22;
    sl = strrchr(ini, '\\'); if (!sl) return 22; strcpy(sl + 1, "TheMoviesGfx.ini");
    v = (int)GetPrivateProfileIntA("Gfx", "ExportCRF", 22, ini);
    return v < 14 ? 14 : v > 30 ? 30 : v;
}
static int g_force_wmv;
static void abort_encode(WF *f)
{
    if (f->venc) avcodec_free_context(&f->venc);
    if (f->aenc) avcodec_free_context(&f->aenc);
    if (f->swr) swr_free(&f->swr);
    if (f->fifo) { av_audio_fifo_free(f->fifo); f->fifo = NULL; }
    if (f->vframe) av_frame_free(&f->vframe);
    if (f->aframe) av_frame_free(&f->aframe);
    if (f->pkt) av_packet_free(&f->pkt);
    if (f->oc) { if (f->oc->pb) avio_closep(&f->oc->pb); avformat_free_context(f->oc); f->oc = NULL; }
    f->vst = f->ast = NULL;
}
static int begin_encode_fmt(WF *f, int mp4);
static int begin_encode(WF *f)
{
    int mp4 = !g_force_wmv && want_mp4() && avcodec_find_encoder_by_name("libx264") && avcodec_find_encoder_by_name("aac") && av_guess_format("mp4", NULL, NULL);
    int r = begin_encode_fmt(f, mp4);
    if (r < 0 && mp4) { logmsg("mp4 export failed to start, falling back to wmv"); abort_encode(f); r = begin_encode_fmt(f, 0); }
    return r;
}
static int begin_encode_fmt(WF *f, int mp4)
{
    char path[1024];
    int i;
    if (f->started || !f->file) return -1;
    WideCharToMultiByte(CP_UTF8, 0, f->file, -1, path, sizeof path, NULL, NULL);
    if (mp4) mp4_path(path);
    logmsg("begin_encode (%s) -> %s", mp4 ? "mp4" : "wmv", path);
    av_log_set_level(AV_LOG_QUIET);
    if (avformat_alloc_output_context2(&f->oc, NULL, mp4 ? "mp4" : "asf", path) < 0) { logmsg("alloc output ctx failed"); return -1; }
    WP *vp = NULL, *ap = NULL;
    for (i = 0; i < f->npins; i++) { WP *p = f->order[i]; if (!p->peer) continue; if (p->kind) vp = p; else ap = p; }
    if (vp) {
        const AVCodec *c = avcodec_find_encoder_by_name(mp4 ? "libx264" : "wmv2");
        if (!c) { logmsg("no video encoder"); return -1; }
        f->venc = avcodec_alloc_context3(c);
        int w = f->pw > 0 ? f->pw : vp->w, h = f->ph > 0 ? f->ph : vp->h;
        w &= ~1; h &= ~1;
        f->ew = w; f->eh = h;
        LONGLONG avg = f->pavg > 0 ? f->pavg : (vp->avg > 0 ? vp->avg : 333333);
        f->venc->width = w; f->venc->height = h; f->venc->pix_fmt = AV_PIX_FMT_YUV420P;
        AVRational tb = {(int)avg, 10000000};
        av_reduce(&tb.num, &tb.den, tb.num, tb.den, 1 << 30);
        if (mp4) { double fps = (double)tb.den / tb.num; int rr = (int)(fps + 0.5); if (rr > 0 && fabs(fps - rr) < 0.005 * rr) { tb.num = 1; tb.den = rr; } } /* 30.00003 fps from the profile -> 30/1: mp4 players dislike a 10000000/333333 rate */
        f->venc->time_base = tb; f->venc->framerate = (AVRational){tb.den, tb.num};
        f->venc->bit_rate = f->vbitrate ? f->vbitrate : 2000000;
        f->venc->gop_size = (int)((double)tb.den / tb.num * 5.0 + 0.5);
        if (f->venc->gop_size < 1) f->venc->gop_size = 30;
        f->venc->max_b_frames = 0;
        f->venc->thread_count = mp4 ? 3 : 1; /* not 0: auto = ~18 threads on 12 cores, too much memory for a 32-bit process */
        if (mp4) { f->venc->bit_rate = 0; /* constant quality instead of the WMV profile bitrate */ }
        if (f->oc->oformat->flags & AVFMT_GLOBALHEADER) f->venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        { AVDictionary *o = NULL; if (mp4) { av_dict_set(&o, "preset", "veryfast", 0); { char cv[16]; sprintf(cv, "%d", export_crf()); av_dict_set(&o, "crf", cv, 0); } av_dict_set(&o, "profile", "high", 0); av_dict_set(&o, "x264-params", "rc-lookahead=10:sync-lookahead=0:lookahead-threads=1", 0); }
          int oe = avcodec_open2(f->venc, c, &o); av_dict_free(&o); if (oe < 0) { logmsg("video encoder open failed"); return -1; } }
        f->vst = avformat_new_stream(f->oc, NULL);
        avcodec_parameters_from_context(f->vst->codecpar, f->venc);
        f->vst->time_base = f->venc->time_base;
        f->vframe = av_frame_alloc();
        f->vframe->format = AV_PIX_FMT_YUV420P; f->vframe->width = w; f->vframe->height = h;
        av_frame_get_buffer(f->vframe, 0);
        logmsg("video encoder %dx%d tb=%d/%d bitrate=%lld gop=%d", w, h, tb.num, tb.den, (long long)f->venc->bit_rate, f->venc->gop_size);
    }
    if (ap) {
        const AVCodec *c = avcodec_find_encoder_by_name(mp4 ? "aac" : "wmav2");
        if (!c) { logmsg("no audio encoder"); return -1; }
        f->aenc = avcodec_alloc_context3(c);
        int ch = f->pach ? f->pach : ap->ach; if (ch > 2) ch = 2; if (ch < 1) ch = 2;
        int rate = f->parate ? f->parate : ap->arate;
        f->aenc->sample_rate = rate; f->aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&f->aenc->ch_layout, ch);
        f->aenc->bit_rate = f->abitrate ? f->abitrate : 128000;
        if (f->aenc->bit_rate < 32000) f->aenc->bit_rate = 32000; /* ffmpeg's wmav2 refuses < 24 kbps (Upload profile asks for 20 kbps) */
        f->aenc->time_base = (AVRational){1, rate};
        f->aenc->thread_count = 1;
        if (f->oc->oformat->flags & AVFMT_GLOBALHEADER) f->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(f->aenc, c, NULL) < 0) { logmsg("audio encoder open failed"); return -1; }
        f->ast = avformat_new_stream(f->oc, NULL);
        avcodec_parameters_from_context(f->ast->codecpar, f->aenc);
        f->ast->time_base = f->aenc->time_base;
        AVChannelLayout il; av_channel_layout_default(&il, ap->ach);
        swr_alloc_set_opts2(&f->swr, &f->aenc->ch_layout, AV_SAMPLE_FMT_FLTP, rate, &il, AV_SAMPLE_FMT_S16, ap->arate, 0, NULL);
        swr_init(f->swr);
        f->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, ch, 1);
        f->aframe = av_frame_alloc();
        logmsg("audio encoder %d Hz %d ch bitrate=%lld frame_size=%d", rate, ch, (long long)f->aenc->bit_rate, f->aenc->frame_size);
    }
    f->pkt = av_packet_alloc();
    if (avio_open(&f->oc->pb, path, AVIO_FLAG_WRITE) < 0) { logmsg("avio_open failed"); return -1; }
    AVDictionary *mo = NULL; if (mp4) av_dict_set(&mo, "movflags", "+faststart", 0);
    int r = avformat_write_header(f->oc, &mo); av_dict_free(&mo);
    if (r < 0) { logmsg("write_header failed %d", r); return -1; }
    f->started = 1; f->finished = 0; f->vlast = -1; f->apts = 0; f->nv = f->na = 0;
    return 0;
}
static void encode_audio_fifo(WF *f, int flush)
{
    int fs = f->aenc->frame_size > 0 ? f->aenc->frame_size : 2048;
    while (av_audio_fifo_size(f->fifo) >= fs || (flush && av_audio_fifo_size(f->fifo) > 0)) {
        int n = av_audio_fifo_size(f->fifo); if (n > fs) n = fs;
        av_frame_unref(f->aframe);
        f->aframe->nb_samples = fs; f->aframe->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&f->aframe->ch_layout, &f->aenc->ch_layout);
        f->aframe->sample_rate = f->aenc->sample_rate;
        av_frame_get_buffer(f->aframe, 0);
        av_frame_make_writable(f->aframe);
        av_audio_fifo_read(f->fifo, (void **)f->aframe->data, n);
        if (n < fs) for (int c = 0; c < f->aenc->ch_layout.nb_channels; c++) memset(f->aframe->data[c] + n * 4, 0, (fs - n) * 4);
        f->aframe->pts = f->apts; f->apts += fs;
        avcodec_send_frame(f->aenc, f->aframe);
        write_packets(f, f->aenc, f->ast);
    }
}
static void finish_encode(WF *f)
{
    if (!f->started || f->finished) return;
    logmsg("finish_encode (video frames %ld, audio chunks %ld)", f->nv, f->na);
    f->finished = 1;
    if (f->venc) { avcodec_send_frame(f->venc, NULL); write_packets(f, f->venc, f->vst); }
    if (f->aenc) { encode_audio_fifo(f, 1); avcodec_send_frame(f->aenc, NULL); write_packets(f, f->aenc, f->ast); }
    av_write_trailer(f->oc);
    avio_closep(&f->oc->pb);
    if (f->venc) avcodec_free_context(&f->venc);
    if (f->aenc) avcodec_free_context(&f->aenc);
    if (f->sws) { sws_freeContext(f->sws); f->sws = NULL; }
    if (f->swr) swr_free(&f->swr);
    if (f->fifo) { av_audio_fifo_free(f->fifo); f->fifo = NULL; }
    if (f->vframe) av_frame_free(&f->vframe);
    if (f->aframe) av_frame_free(&f->aframe);
    if (f->pkt) av_packet_free(&f->pkt);
    avformat_free_context(f->oc); f->oc = NULL;
    f->started = 0; f->vst = f->ast = NULL;
    logmsg("file finalized");
}

static HRESULT receive_video(WP *p, IMediaSample *ms)
{
    WF *f = p->f;
    BYTE *buf; LONG len = IMediaSample_GetActualDataLength(ms);
    IMediaSample_GetPointer(ms, &buf);
    int stride = ((p->w * p->bpp + 31) / 32) * 4;
    if (len < stride * p->h) return S_OK;
    REFERENCE_TIME a = 0, b = 0;
    int have = SUCCEEDED(IMediaSample_GetTime(ms, &a, &b));
    EnterCriticalSection(&f->lock);
    if (!f->started || !f->venc) { LeaveCriticalSection(&f->lock); return VFW_E_WRONG_STATE_; }
    enum AVPixelFormat sf = p->bpp == 32 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR24;
    f->sws = sws_getCachedContext(f->sws, p->w, p->h, sf, f->ew, f->eh, AV_PIX_FMT_YUV420P, SWS_BICUBIC | SWS_ACCURATE_RND | SWS_BITEXACT, NULL, NULL, NULL);
    const uint8_t *sp[4] = {buf, 0, 0, 0}; int ss[4] = {stride, 0, 0, 0};
    if (p->flip) { sp[0] = buf + (size_t)stride * (p->h - 1); ss[0] = -stride; }
    av_frame_make_writable(f->vframe);
    sws_scale(f->sws, sp, ss, 0, p->h, f->vframe->data, f->vframe->linesize);
    LONGLONG avg = (LONGLONG)f->venc->time_base.num * 10000000LL / f->venc->time_base.den;
    LONGLONG idx = have ? (a + avg / 2) / avg : f->nv;
    if (idx <= f->vlast) idx = f->vlast + 1;
    f->vlast = idx;
    f->vframe->pts = idx;
    f->nv++;
    avcodec_send_frame(f->venc, f->vframe);
    write_packets(f, f->venc, f->vst);
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT receive_audio(WP *p, IMediaSample *ms)
{
    WF *f = p->f;
    BYTE *buf; LONG len = IMediaSample_GetActualDataLength(ms);
    IMediaSample_GetPointer(ms, &buf);
    int n = len / (p->ach * 2);
    if (n <= 0) return S_OK;
    EnterCriticalSection(&f->lock);
    if (!f->started || !f->aenc) { LeaveCriticalSection(&f->lock); return VFW_E_WRONG_STATE_; }
    int outn = (int)av_rescale_rnd(swr_get_delay(f->swr, p->arate) + n, f->aenc->sample_rate, p->arate, AV_ROUND_UP) + 32;
    uint8_t **out = NULL; int ls;
    av_samples_alloc_array_and_samples(&out, &ls, f->aenc->ch_layout.nb_channels, outn, AV_SAMPLE_FMT_FLTP, 0);
    const uint8_t *in[1] = {buf};
    int got = swr_convert(f->swr, out, outn, in, n);
    if (got > 0) { av_audio_fifo_write(f->fifo, (void **)out, got); f->na++; encode_audio_fifo(f, 0); }
    av_freep(&out[0]); av_freep(&out);
    LeaveCriticalSection(&f->lock);
    return S_OK;
}

/* ---------- pins ---------- */
static int mt_ok(WP *p, const AM_MEDIA_TYPE *m)
{
    if (!m) return 0;
    if (p->kind) {
        if (!IsEqualGUID(&m->majortype, &G_Video) || !IsEqualGUID(&m->formattype, &G_FmtVideo)) return 0;
        if (!IsEqualGUID(&m->subtype, &G_RGB24) && !IsEqualGUID(&m->subtype, &G_RGB32)) return 0;
        return m->cbFormat >= sizeof(VIH_);
    }
    if (!IsEqualGUID(&m->majortype, &G_Audio) || !IsEqualGUID(&m->formattype, &G_FmtWave) || !IsEqualGUID(&m->subtype, &G_PCM)) return 0;
    if (m->cbFormat < sizeof(WAVEFORMATEX)) return 0;
    WAVEFORMATEX *w = (WAVEFORMATEX *)m->pbFormat;
    return w->wFormatTag == WAVE_FORMAT_PCM && w->wBitsPerSample == 16 && w->nChannels >= 1 && w->nChannels <= 8;
}
static HRESULT WINAPI WPN_QI(IPin *t, REFIID r, void **o)
{
    WP *p = P_PIN(t);
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IPin)) *o = &p->pin;
    else if (IsEqualGUID(r, &IID_IMemInputPin)) *o = &p->mip;
    else { logmsg("WriterPin QI %s -> E_NOINTERFACE", guidstr(r)); *o = NULL; return E_NOINTERFACE; }
    IBaseFilter_AddRef(&p->f->bf);
    return S_OK;
}
static ULONG WINAPI WPN_AddRef(IPin *t) { return IBaseFilter_AddRef(&P_PIN(t)->f->bf); }
static ULONG WINAPI WPN_Release(IPin *t) { return IBaseFilter_Release(&P_PIN(t)->f->bf); }
static HRESULT WINAPI WPN_Connect(IPin *t, IPin *r, const AM_MEDIA_TYPE *m) { return E_UNEXPECTED; }
static HRESULT WINAPI WPN_ReceiveConnection(IPin *t, IPin *conn, const AM_MEDIA_TYPE *m)
{
    WP *p = P_PIN(t);
    logmsg("writer pin %d: ReceiveConnection major=%s sub=%s fmt=%s cb=%lu", p->kind, m ? guidstr(&m->majortype) : "-", m ? guidstr(&m->subtype) : "-", m ? guidstr(&m->formattype) : "-", m ? m->cbFormat : 0);
    if (p->peer) return VFW_E_ALREADY_CONNECTED_;
    if (p->f->state != State_Stopped) return VFW_E_NOT_STOPPED_;
    if (!mt_ok(p, m)) return VFW_E_TYPE_NOT_ACCEPTED_;
    if (p->kind) {
        VIH_ *v = (VIH_ *)m->pbFormat;
        p->w = v->bmi.biWidth; p->h = abs(v->bmi.biHeight); p->flip = v->bmi.biHeight > 0; p->bpp = v->bmi.biBitCount;
        if (p->bpp != 24 && p->bpp != 32) return VFW_E_TYPE_NOT_ACCEPTED_;
        p->avg = v->AvgTimePerFrame;
        logmsg("   video %dx%d bpp=%d flip=%d avg=%lld", p->w, p->h, p->bpp, p->flip, (long long)p->avg);
    } else {
        WAVEFORMATEX *w = (WAVEFORMATEX *)m->pbFormat;
        p->ach = w->nChannels; p->arate = w->nSamplesPerSec;
        logmsg("   audio %d ch %d Hz", p->ach, p->arate);
    }
    IPin_AddRef(conn); p->peer = conn;
    return S_OK;
}
static HRESULT WINAPI WPN_Disconnect(IPin *t)
{
    WP *p = P_PIN(t);
    logmsg("writer pin %d: Disconnect", p->kind);
    if (p->f->state != State_Stopped) return VFW_E_NOT_STOPPED_;
    if (!p->peer) return S_FALSE;
    IPin_Release(p->peer); p->peer = NULL; return S_OK;
}
static HRESULT WINAPI WPN_ConnectedTo(IPin *t, IPin **o)
{
    WP *p = P_PIN(t);
    if (!p->peer) { *o = NULL; return VFW_E_NOT_CONNECTED_; }
    IPin_AddRef(p->peer); *o = p->peer; return S_OK;
}
static HRESULT WINAPI WPN_ConnectionMediaType(IPin *t, AM_MEDIA_TYPE *m)
{
    WP *p = P_PIN(t);
    memset(m, 0, sizeof *m);
    if (!p->peer) return VFW_E_NOT_CONNECTED_;
    if (p->kind) {
        VIH_ *v = CoTaskMemAlloc(sizeof *v); memset(v, 0, sizeof *v);
        v->bmi.biSize = sizeof(BITMAPINFOHEADER); v->bmi.biWidth = p->w; v->bmi.biHeight = p->flip ? p->h : -p->h;
        v->bmi.biPlanes = 1; v->bmi.biBitCount = p->bpp; v->AvgTimePerFrame = p->avg;
        m->majortype = G_Video; m->subtype = p->bpp == 32 ? G_RGB32 : G_RGB24; m->formattype = G_FmtVideo; m->cbFormat = sizeof *v; m->pbFormat = (BYTE *)v;
    } else {
        WAVEFORMATEX *w = CoTaskMemAlloc(sizeof *w); memset(w, 0, sizeof *w);
        w->wFormatTag = WAVE_FORMAT_PCM; w->nChannels = p->ach; w->nSamplesPerSec = p->arate; w->wBitsPerSample = 16;
        w->nBlockAlign = p->ach * 2; w->nAvgBytesPerSec = w->nBlockAlign * p->arate;
        m->majortype = G_Audio; m->subtype = G_PCM; m->formattype = G_FmtWave; m->cbFormat = sizeof *w; m->pbFormat = (BYTE *)w;
    }
    return S_OK;
}
static HRESULT WINAPI WPN_QueryPinInfo(IPin *t, PIN_INFO *i)
{
    WP *p = P_PIN(t);
    i->pFilter = &p->f->bf; IBaseFilter_AddRef(i->pFilter);
    i->dir = PINDIR_INPUT; lstrcpyW(i->achName, p->name); return S_OK;
}
static HRESULT WINAPI WPN_QueryDirection(IPin *t, PIN_DIRECTION *d) { *d = PINDIR_INPUT; return S_OK; }
static HRESULT WINAPI WPN_QueryId(IPin *t, LPWSTR *id)
{
    WP *p = P_PIN(t);
    *id = CoTaskMemAlloc((lstrlenW(p->name) + 1) * 2); lstrcpyW(*id, p->name); return S_OK;
}
static HRESULT WINAPI WPN_QueryAccept(IPin *t, const AM_MEDIA_TYPE *m) { return mt_ok(P_PIN(t), m) ? S_OK : S_FALSE; }

typedef struct { IEnumMediaTypes e; LONG ref; } EMT0;
static HRESULT WINAPI E0_QI(IEnumMediaTypes *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IEnumMediaTypes)) { *o = t; ((EMT0 *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI E0_AddRef(IEnumMediaTypes *t) { return ++((EMT0 *)t)->ref; }
static ULONG WINAPI E0_Release(IEnumMediaTypes *t) { EMT0 *e = (EMT0 *)t; if (!--e->ref) { free(e); return 0; } return e->ref; }
static HRESULT WINAPI E0_Next(IEnumMediaTypes *t, ULONG n, AM_MEDIA_TYPE **m, ULONG *g) { if (g) *g = 0; return S_FALSE; }
static HRESULT WINAPI E0_Skip(IEnumMediaTypes *t, ULONG n) { return S_FALSE; }
static HRESULT WINAPI E0_Reset(IEnumMediaTypes *t) { return S_OK; }
static HRESULT WINAPI E0_Clone(IEnumMediaTypes *t, IEnumMediaTypes **o) { EMT0 *e = calloc(1, sizeof *e); e->e.lpVtbl = t->lpVtbl; e->ref = 1; *o = &e->e; return S_OK; }
static const IEnumMediaTypesVtbl e0_vtbl = {E0_QI, E0_AddRef, E0_Release, E0_Next, E0_Skip, E0_Reset, E0_Clone};
static HRESULT WINAPI WPN_EnumMediaTypes(IPin *t, IEnumMediaTypes **o)
{
    EMT0 *e = calloc(1, sizeof *e); e->e.lpVtbl = &e0_vtbl; e->ref = 1; *o = &e->e; return S_OK;
}
static HRESULT WINAPI WPN_QueryInternalConnections(IPin *t, IPin **p, ULONG *n) { return E_NOTIMPL; }
static void notify_complete(WF *f)
{
    IMediaEventSink *sink = NULL;
    if (f->graph && SUCCEEDED(IFilterGraph_QueryInterface(f->graph, &IID_IMediaEventSink, (void **)&sink))) {
        logmsg("sending EC_COMPLETE");
        IMediaEventSink_Notify(sink, EC_COMPLETE, S_OK, (LONG_PTR)&f->bf);
        IMediaEventSink_Release(sink);
    }
}
static HRESULT WINAPI WPN_EndOfStream(IPin *t)
{
    WP *p = P_PIN(t);
    WF *f = p->f;
    int all = 1, i;
    logmsg("writer pin %d: EndOfStream", p->kind);
    EnterCriticalSection(&f->lock);
    p->eos = 1;
    for (i = 0; i < f->npins; i++) if (f->order[i]->peer && !f->order[i]->eos) all = 0;
    if (all) finish_encode(f);
    LeaveCriticalSection(&f->lock);
    if (all) notify_complete(f);
    return S_OK;
}
static HRESULT WINAPI WPN_BeginFlush(IPin *t) { return S_OK; }
static HRESULT WINAPI WPN_EndFlush(IPin *t) { return S_OK; }
static HRESULT WINAPI WPN_NewSegment(IPin *t, REFERENCE_TIME a, REFERENCE_TIME b, double r) { return S_OK; }
static const IPinVtbl wpin_vtbl = {WPN_QI, WPN_AddRef, WPN_Release, WPN_Connect, WPN_ReceiveConnection, WPN_Disconnect, WPN_ConnectedTo,
    WPN_ConnectionMediaType, WPN_QueryPinInfo, WPN_QueryDirection, WPN_QueryId, WPN_QueryAccept, WPN_EnumMediaTypes,
    WPN_QueryInternalConnections, WPN_EndOfStream, WPN_BeginFlush, WPN_EndFlush, WPN_NewSegment};

/* IMemInputPin */
static HRESULT WINAPI MIP_QI(IMemInputPin *t, REFIID r, void **o) { return WPN_QI(&P_MIP(t)->pin, r, o); }
static ULONG WINAPI MIP_AddRef(IMemInputPin *t) { return WPN_AddRef(&P_MIP(t)->pin); }
static ULONG WINAPI MIP_Release(IMemInputPin *t) { return WPN_Release(&P_MIP(t)->pin); }
static HRESULT WINAPI MIP_GetAllocator(IMemInputPin *t, IMemAllocator **o)
{
    return CoCreateInstance(&G_MemAllocClsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMemAllocator, (void **)o);
}
static HRESULT WINAPI MIP_NotifyAllocator(IMemInputPin *t, IMemAllocator *a, BOOL ro) { return S_OK; }
static HRESULT WINAPI MIP_GetAllocatorRequirements(IMemInputPin *t, ALLOCATOR_PROPERTIES *p) { return E_NOTIMPL; }
static HRESULT WINAPI MIP_Receive(IMemInputPin *t, IMediaSample *ms)
{
    WP *p = P_MIP(t);
    if (++p->nrecv <= 3 || p->nrecv % 300 == 0) logmsg("writer pin %d: Receive #%ld len=%ld", p->kind, p->nrecv, IMediaSample_GetActualDataLength(ms));
    if (p->f->state == State_Stopped) return VFW_E_WRONG_STATE_;
    return p->kind ? receive_video(p, ms) : receive_audio(p, ms);
}
static HRESULT WINAPI MIP_ReceiveMultiple(IMemInputPin *t, IMediaSample **s, LONG n, LONG *done)
{
    LONG i; HRESULT hr = S_OK;
    for (i = 0; i < n && hr == S_OK; i++) hr = MIP_Receive(t, s[i]);
    *done = i; return hr;
}
static HRESULT WINAPI MIP_ReceiveCanBlock(IMemInputPin *t) { return S_OK; }
static const IMemInputPinVtbl mip_vtbl = {MIP_QI, MIP_AddRef, MIP_Release, MIP_GetAllocator, MIP_NotifyAllocator, MIP_GetAllocatorRequirements,
    MIP_Receive, MIP_ReceiveMultiple, MIP_ReceiveCanBlock};

/* ---------- IEnumPins ---------- */
typedef struct { IEnumPins e; LONG ref; WF *f; int idx; } EP;
static HRESULT WINAPI EP_QI(IEnumPins *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IEnumPins)) { *o = t; ((EP *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI EP_AddRef(IEnumPins *t) { return ++((EP *)t)->ref; }
static ULONG WINAPI EP_Release(IEnumPins *t) { EP *e = (EP *)t; if (!--e->ref) { IBaseFilter_Release(&e->f->bf); free(e); return 0; } return e->ref; }
static HRESULT WINAPI EP_Next(IEnumPins *t, ULONG n, IPin **pins, ULONG *got)
{
    EP *e = (EP *)t; ULONG k = 0;
    while (k < n && e->idx < e->f->npins) { WP *p = e->f->order[e->idx++]; IPin_AddRef(&p->pin); pins[k++] = &p->pin; }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI EP_Skip(IEnumPins *t, ULONG n) { EP *e = (EP *)t; e->idx += n; return e->idx > e->f->npins ? S_FALSE : S_OK; }
static HRESULT WINAPI EP_Reset(IEnumPins *t) { ((EP *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI EP_Clone(IEnumPins *t, IEnumPins **o) { EP *e = (EP *)t, *n = calloc(1, sizeof *n); *n = *e; n->ref = 1; IBaseFilter_AddRef(&n->f->bf); *o = &n->e; return S_OK; }
static const IEnumPinsVtbl ep_vtbl = {EP_QI, EP_AddRef, EP_Release, EP_Next, EP_Skip, EP_Reset, EP_Clone};

/* ---------- IBaseFilter ---------- */
static HRESULT WINAPI BF_QI(IBaseFilter *t, REFIID r, void **o)
{
    WF *f = W_BF(t);
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IPersist) || IsEqualGUID(r, &IID_IMediaFilter) || IsEqualGUID(r, &IID_IBaseFilter)) *o = &f->bf;
    else if (IsEqualGUID(r, &IID_IFileSinkFilter) || IsEqualGUID(r, &IID_IFileSinkFilter2)) *o = &f->fs;
    else if (IsEqualGUID(r, &G_IConfigAsfWriter)) *o = &f->cw;
    else if (IsEqualGUID(r, &G_IMiscFlags)) *o = &f->mf;
    else { logmsg("Writer QI %s -> E_NOINTERFACE", guidstr(r)); *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&f->ref);
    return S_OK;
}
static ULONG WINAPI BF_AddRef(IBaseFilter *t) { return InterlockedIncrement(&W_BF(t)->ref); }
static ULONG WINAPI BF_Release(IBaseFilter *t)
{
    WF *f = W_BF(t);
    LONG n = InterlockedDecrement(&f->ref);
    if (!n) {
        logmsg("Writer destroyed");
        EnterCriticalSection(&f->lock); finish_encode(f); LeaveCriticalSection(&f->lock);
        for (int i = 0; i < 2; i++) if (f->pins[i].peer) IPin_Release(f->pins[i].peer);
        free(f->file);
        DeleteCriticalSection(&f->lock);
        free(f);
    }
    return n;
}
static HRESULT WINAPI BF_GetClassID(IBaseFilter *t, CLSID *c) { *c = CLSID_WMAsfWriter_; return S_OK; }
static HRESULT WINAPI BF_Stop(IBaseFilter *t)
{
    WF *f = W_BF(t);
    logmsg("Writer Stop");
    EnterCriticalSection(&f->lock);
    f->state = State_Stopped;
    finish_encode(f);
    for (int i = 0; i < f->npins; i++) { f->order[i]->eos = 0; f->order[i]->nrecv = 0; }
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_Pause(IBaseFilter *t)
{
    WF *f = W_BF(t);
    logmsg("Writer Pause (state %d)", f->state);
    EnterCriticalSection(&f->lock);
    if (f->state == State_Stopped) begin_encode(f);
    f->state = State_Paused;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_Run(IBaseFilter *t, REFERENCE_TIME s)
{
    WF *f = W_BF(t);
    logmsg("Writer Run (state %d)", f->state);
    EnterCriticalSection(&f->lock);
    if (f->state == State_Stopped) begin_encode(f);
    f->state = State_Running;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_GetState(IBaseFilter *t, DWORD ms, FILTER_STATE *s) { *s = W_BF(t)->state; return S_OK; }
static HRESULT WINAPI BF_SetSyncSource(IBaseFilter *t, IReferenceClock *c) { return S_OK; }
static HRESULT WINAPI BF_GetSyncSource(IBaseFilter *t, IReferenceClock **c) { *c = NULL; return S_OK; }
static HRESULT WINAPI BF_EnumPins(IBaseFilter *t, IEnumPins **o)
{
    EP *e = calloc(1, sizeof *e);
    e->e.lpVtbl = &ep_vtbl; e->ref = 1; e->f = W_BF(t); IBaseFilter_AddRef(t);
    *o = &e->e; return S_OK;
}
static HRESULT WINAPI BF_FindPin(IBaseFilter *t, LPCWSTR id, IPin **o)
{
    WF *f = W_BF(t);
    for (int i = 0; i < f->npins; i++) if (!lstrcmpiW(f->order[i]->name, id)) { IPin_AddRef(&f->order[i]->pin); *o = &f->order[i]->pin; return S_OK; }
    *o = NULL; return VFW_E_NOT_FOUND_;
}
static HRESULT WINAPI BF_QueryFilterInfo(IBaseFilter *t, FILTER_INFO *i)
{
    WF *f = W_BF(t);
    lstrcpyW(i->achName, f->gname);
    i->pGraph = f->graph; if (f->graph) IFilterGraph_AddRef(f->graph);
    return S_OK;
}
static HRESULT WINAPI BF_JoinFilterGraph(IBaseFilter *t, IFilterGraph *g, LPCWSTR n)
{
    WF *f = W_BF(t);
    f->graph = g;
    if (n) lstrcpynW(f->gname, n, 128); else f->gname[0] = 0;
    return S_OK;
}
static HRESULT WINAPI BF_QueryVendorInfo(IBaseFilter *t, LPWSTR *v) { return E_NOTIMPL; }
static const IBaseFilterVtbl bf_vtbl = {BF_QI, BF_AddRef, BF_Release, BF_GetClassID, BF_Stop, BF_Pause, BF_Run, BF_GetState,
    BF_SetSyncSource, BF_GetSyncSource, BF_EnumPins, BF_FindPin, BF_QueryFilterInfo, BF_JoinFilterGraph, BF_QueryVendorInfo};

/* IFileSinkFilter2 */
static HRESULT WINAPI FS_QI(IFileSinkFilter2 *t, REFIID r, void **o) { return BF_QI(&W_FS(t)->bf, r, o); }
static ULONG WINAPI FS_AddRef(IFileSinkFilter2 *t) { return BF_AddRef(&W_FS(t)->bf); }
static ULONG WINAPI FS_Release(IFileSinkFilter2 *t) { return BF_Release(&W_FS(t)->bf); }
static HRESULT WINAPI FS_SetFileName(IFileSinkFilter2 *t, LPCOLESTR fn, const AM_MEDIA_TYPE *m)
{
    WF *f = W_FS(t);
    char u[1024]; WideCharToMultiByte(CP_UTF8, 0, fn, -1, u, sizeof u, NULL, NULL);
    logmsg("SetFileName(%s)", u);
    free(f->file); f->file = _wcsdup(fn);
    return S_OK;
}
static HRESULT WINAPI FS_GetCurFile(IFileSinkFilter2 *t, LPOLESTR *fn, AM_MEDIA_TYPE *m)
{
    WF *f = W_FS(t);
    if (f->file) { *fn = CoTaskMemAlloc((lstrlenW(f->file) + 1) * 2); lstrcpyW(*fn, f->file); } else *fn = NULL;
    if (m) memset(m, 0, sizeof *m);
    return S_OK;
}
static HRESULT WINAPI FS_SetMode(IFileSinkFilter2 *t, DWORD m) { W_FS(t)->mode = m; return S_OK; }
static HRESULT WINAPI FS_GetMode(IFileSinkFilter2 *t, DWORD *m) { *m = W_FS(t)->mode; return S_OK; }
static const IFileSinkFilter2Vtbl fs_vtbl = {FS_QI, FS_AddRef, FS_Release, FS_SetFileName, FS_GetCurFile, FS_SetMode, FS_GetMode};

/* IAMFilterMiscFlags */
static HRESULT WINAPI MF_QI(IAMFilterMiscFlags *t, REFIID r, void **o) { return BF_QI(&W_MF(t)->bf, r, o); }
static ULONG WINAPI MF_AddRef(IAMFilterMiscFlags *t) { return BF_AddRef(&W_MF(t)->bf); }
static ULONG WINAPI MF_Release(IAMFilterMiscFlags *t) { return BF_Release(&W_MF(t)->bf); }
static ULONG WINAPI MF_Get(IAMFilterMiscFlags *t) { return AM_FILTER_MISC_FLAGS_IS_RENDERER; }
static const IAMFilterMiscFlagsVtbl mf_vtbl = {MF_QI, MF_AddRef, MF_Release, MF_Get};

/* IConfigAsfWriter */
static void add_pin(WF *f, int kind)
{
    WP *p = &f->pins[f->npins];
    memset(p, 0, sizeof *p);
    p->pin.lpVtbl = &wpin_vtbl; p->mip.lpVtbl = &mip_vtbl; p->f = f; p->kind = kind;
    wsprintfW(p->name, kind ? L"Video Input %02d" : L"Audio Input %02d", f->npins + 1);
    f->order[f->npins] = p; f->npins++;
}
static HRESULT WINAPI CW_QI(IConfigAsfWriterX *t, REFIID r, void **o) { return BF_QI(&W_CW(t)->bf, r, o); }
static ULONG WINAPI CW_AddRef(IConfigAsfWriterX *t) { return BF_AddRef(&W_CW(t)->bf); }
static ULONG WINAPI CW_Release(IConfigAsfWriterX *t) { return BF_Release(&W_CW(t)->bf); }
static HRESULT WINAPI CW_CfgId(IConfigAsfWriterX *t, DWORD id)
{
    WF *f = W_CW(t);
    logmsg("ConfigureFilterUsingProfileId(%lu)", id);
    if (!f->npins) { add_pin(f, 0); add_pin(f, 1); }
    return S_OK;
}
static HRESULT WINAPI CW_GetId(IConfigAsfWriterX *t, DWORD *id) { *id = 0; return S_OK; }
static HRESULT WINAPI CW_CfgGuid(IConfigAsfWriterX *t, REFGUID g)
{
    WF *f = W_CW(t);
    logmsg("ConfigureFilterUsingProfileGuid(%s)", guidstr(g));
    if (!f->npins) { add_pin(f, 0); add_pin(f, 1); }
    return S_OK;
}
static HRESULT WINAPI CW_GetGuid(IConfigAsfWriterX *t, GUID *g) { *g = GUID_NULL; return S_OK; }
static HRESULT WINAPI CW_CfgProfile(IConfigAsfWriterX *t, IWMProfile *prof)
{
    WF *f = W_CW(t);
    DWORD n = 0, i;
    logmsg("ConfigureFilterUsingProfile(%p)", prof);
    if (!prof) return E_POINTER;
    if (f->npins) return VFW_E_NOT_STOPPED_;
    IWMProfile_GetStreamCount(prof, &n);
    for (i = 0; i < n; i++) {
        IWMStreamConfig *sc = NULL; GUID type; DWORD br = 0;
        if (FAILED(IWMProfile_GetStream(prof, i, &sc))) continue;
        IWMStreamConfig_GetStreamType(sc, &type);
        IWMStreamConfig_GetBitrate(sc, &br);
        IWMMediaProps *mp = NULL;
        BYTE mtbuf[2048]; DWORD cb = sizeof mtbuf; int have = 0;
        WM_MEDIA_TYPE *mt = (WM_MEDIA_TYPE *)mtbuf;
        if (SUCCEEDED(IWMStreamConfig_QueryInterface(sc, &G_IWMMediaProps_, (void **)&mp))) {
            have = SUCCEEDED(IWMMediaProps_GetMediaType(mp, mt, &cb));
            IWMMediaProps_Release(mp);
        }
        if (IsEqualGUID(&type, &G_Video)) {
            f->vbitrate = br;
            if (have && mt->cbFormat >= sizeof(VIH_)) {
                VIH_ *v = (VIH_ *)mt->pbFormat;
                f->pw = v->bmi.biWidth; f->ph = abs(v->bmi.biHeight); f->pavg = v->AvgTimePerFrame;
            }
            struct { const void *vt; } *vp = NULL;
            if (SUCCEEDED(IWMStreamConfig_QueryInterface(sc, &G_IWMVideoMediaProps, (void **)&vp))) {
                /* GetQuality is vtable slot 9 */
                typedef HRESULT (WINAPI *GQ)(void *, DWORD *);
                typedef ULONG (WINAPI *REL)(void *);
                DWORD q = 0; ((GQ)((void **)vp->vt)[8])(vp, &q); f->quality = q;
                ((REL)((void **)vp->vt)[2])(vp);
            }
            add_pin(f, 1);
            logmsg("   profile video: %dx%d bitrate=%lu avg=%lld quality=%lu", f->pw, f->ph, br, (long long)f->pavg, f->quality);
        } else if (IsEqualGUID(&type, &G_Audio)) {
            f->abitrate = br;
            if (have && mt->cbFormat >= sizeof(WAVEFORMATEX)) {
                WAVEFORMATEX *w = (WAVEFORMATEX *)mt->pbFormat;
                f->parate = w->nSamplesPerSec; f->pach = w->nChannels;
            }
            add_pin(f, 0);
            logmsg("   profile audio: bitrate=%lu rate=%d ch=%d", br, f->parate, f->pach);
        }
        IWMStreamConfig_Release(sc);
    }
    if (!f->npins) { add_pin(f, 0); add_pin(f, 1); }
    return S_OK;
}
static HRESULT WINAPI CW_GetProfile(IConfigAsfWriterX *t, IWMProfile **p) { *p = NULL; return E_NOTIMPL; }
static HRESULT WINAPI CW_SetIdx(IConfigAsfWriterX *t, BOOL b) { return S_OK; }
static HRESULT WINAPI CW_GetIdx(IConfigAsfWriterX *t, BOOL *b) { *b = TRUE; return S_OK; }
static const IConfigAsfWriterXVtbl cw_vtbl = {CW_QI, CW_AddRef, CW_Release, CW_CfgId, CW_GetId, CW_CfgGuid, CW_GetGuid, CW_CfgProfile, CW_GetProfile, CW_SetIdx, CW_GetIdx};

/* ---------- class factory ---------- */
typedef struct { IClassFactory cf; } CF;
static HRESULT WINAPI CF_QI(IClassFactory *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IClassFactory)) { *o = t; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI CF_AddRef(IClassFactory *t) { return 2; }
static ULONG WINAPI CF_Release(IClassFactory *t) { return 1; }
static HRESULT WINAPI CF_Create(IClassFactory *t, IUnknown *outer, REFIID r, void **o)
{
    logmsg("Writer CreateInstance(%s)", guidstr(r));
    if (outer) return CLASS_E_NOAGGREGATION;
    WF *f = calloc(1, sizeof *f);
    f->bf.lpVtbl = &bf_vtbl; f->fs.lpVtbl = &fs_vtbl; f->cw.lpVtbl = &cw_vtbl; f->mf.lpVtbl = &mf_vtbl; f->ref = 1;
    InitializeCriticalSection(&f->lock);
    f->state = State_Stopped; f->mode = 1;
    HRESULT hr = BF_QI(&f->bf, r, o);
    BF_Release(&f->bf);
    return hr;
}
static HRESULT WINAPI CF_Lock(IClassFactory *t, BOOL l) { return S_OK; }
static const IClassFactoryVtbl cf_vtbl = {CF_QI, CF_AddRef, CF_Release, CF_Create, CF_Lock};
static CF the_cf = {{&cf_vtbl}};

int writer_clsid(REFCLSID c) { return IsEqualGUID(c, &CLSID_WMAsfWriter_); }
HRESULT writer_get_factory(REFIID r, void **o) { return CF_QI(&the_cf.cf, r, o); }
