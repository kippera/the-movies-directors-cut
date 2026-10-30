/* SPDX-License-Identifier: GPL-3.0-or-later */
/* wmvcore.dll replacement shim: IWMSyncReader implemented on FFmpeg.
 * Built for The Movies (2005) which only uses WMCreateSyncReader / WMCreateProfileManager. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WMCreateSyncReader hdr_WMCreateSyncReader
#define WMCreateProfileManager hdr_WMCreateProfileManager
#include <windows.h>
#include <ole2.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wmsdk.h>
#include <nserror.h>
#undef WMCreateSyncReader
#undef WMCreateProfileManager

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define E_NOTFOUND_ASF ((HRESULT)0xC00D07F0)

/* ---------- logging ---------- */
static FILE *logf_;
static CRITICAL_SECTION logcs;
void logmsg(const char *fmt, ...)
{
    va_list ap;
    static LONG dbg = -1; /* logging is OFF unless the marker file C:\\wmvcore.debug exists (the log grew to 25 MB per export session) */
    if (dbg < 0) dbg = (GetFileAttributesA("C:\\wmvcore.debug") != INVALID_FILE_ATTRIBUTES);
    if (!dbg) return;
    if (!logf_) {
        logf_ = fopen("C:\\wmvcore.log", "a");
        if (!logf_) return;
        InitializeCriticalSection(&logcs);
        fprintf(logf_, "---- wmvcore shim loaded (pid %lu)\n", GetCurrentProcessId());
    }
    EnterCriticalSection(&logcs);
    fprintf(logf_, "[%lu] ", GetCurrentThreadId());
    va_start(ap, fmt);
    vfprintf(logf_, fmt, ap);
    va_end(ap);
    fputc('\n', logf_);
    fflush(logf_);
    LeaveCriticalSection(&logcs);
}
const char *guidstr(const GUID *g)
{
    static char b[8][48];
    static LONG i;
    char *s = b[InterlockedIncrement(&i) & 7];
    sprintf(s, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return s;
}

/* ---------- guids ---------- */
static const GUID G_Video = {0x73646976, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_Audio = {0x73647561, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_FmtVideo = {0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID G_FmtWave = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID G_RGB32 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID G_RGB24 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID G_YV12 = {0x32315659, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_I420 = {0x30323449, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_IYUV = {0x56555949, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_YUY2 = {0x32595559, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_PCM = {0x00000001, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_IUnknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID G_IWMSyncReader = {0x9397f121, 0x7705, 0x4dc9, {0xb0, 0x49, 0x98, 0xb6, 0x98, 0x18, 0x84, 0x14}};
static const GUID G_IWMHeaderInfo = {0x96406bda, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_IWMHeaderInfo2 = {0x15cf9781, 0x454e, 0x482e, {0xb3, 0x93, 0x85, 0xfa, 0xe4, 0x87, 0xa8, 0x10}};
static const GUID G_IWMHeaderInfo3 = {0x15cc68e3, 0x27cc, 0x4ecd, {0xb2, 0x22, 0x3f, 0x5d, 0x02, 0xd8, 0x0b, 0xd5}};
static const GUID G_IWMMediaProps = {0x96406bce, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_IWMOutputMediaProps = {0x96406bd7, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_INSSBuffer = {0xe1cd3524, 0x03d7, 0x11d2, {0x9e, 0xed, 0x00, 0x60, 0x97, 0xd2, 0xd7, 0xcf}};
static const GUID G_IWMProfileManager = {0xd16679f2, 0x6ca0, 0x472d, {0x8d, 0x31, 0x2f, 0x5d, 0x55, 0xae, 0xe1, 0x55}};

#pragma pack(push, 1)
typedef struct { LONG left, top, right, bottom; } RC;
typedef struct {
    RC rcSource, rcTarget;
    DWORD dwBitRate, dwBitErrorRate;
    LONGLONG AvgTimePerFrame;
    BITMAPINFOHEADER bmi;
} VIH;
#pragma pack(pop)

/* ---------- video format table ---------- */
typedef struct { const GUID *sub; DWORD comp; WORD bpp; enum AVPixelFormat pix; int flip; int yv; const char *name; } VFmt;
static const VFmt vfmts[] = {
    {&G_RGB32, BI_RGB, 32, AV_PIX_FMT_BGRA, 1, 0, "RGB32"},
    {&G_RGB24, BI_RGB, 24, AV_PIX_FMT_BGR24, 1, 0, "RGB24"},
    {&G_YV12, 0x32315659, 12, AV_PIX_FMT_YUV420P, 0, 1, "YV12"},
    {&G_I420, 0x30323449, 12, AV_PIX_FMT_YUV420P, 0, 0, "I420"},
    {&G_IYUV, 0x56555949, 12, AV_PIX_FMT_YUV420P, 0, 0, "IYUV"},
    {&G_YUY2, 0x32595559, 16, AV_PIX_FMT_YUYV422, 0, 0, "YUY2"},
};
#define NVF ((int)(sizeof(vfmts) / sizeof(vfmts[0])))

/* ---------- samples / buffers ---------- */
typedef struct Sample { struct Sample *next; BYTE *data; DWORD len; QWORD ts, dur; DWORD flags; } Sample;

typedef struct { INSSBuffer iface; LONG ref; BYTE *data; DWORD len, max; } Buf;
#define BUF(p) ((Buf *)(p))
static HRESULT WINAPI Buf_QI(INSSBuffer *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown) || IsEqualGUID(r, &G_INSSBuffer)) { *o = t; InterlockedIncrement(&BUF(t)->ref); return S_OK; }
    logmsg("INSSBuffer QI %s -> E_NOINTERFACE", guidstr(r));
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI Buf_AddRef(INSSBuffer *t) { return InterlockedIncrement(&BUF(t)->ref); }
static ULONG WINAPI Buf_Release(INSSBuffer *t)
{
    LONG n = InterlockedDecrement(&BUF(t)->ref);
    if (!n) { free(BUF(t)->data); free(t); }
    return n;
}
static HRESULT WINAPI Buf_GetLength(INSSBuffer *t, DWORD *l) { *l = BUF(t)->len; return S_OK; }
static HRESULT WINAPI Buf_SetLength(INSSBuffer *t, DWORD l) { if (l > BUF(t)->max) return E_INVALIDARG; BUF(t)->len = l; return S_OK; }
static HRESULT WINAPI Buf_GetMaxLength(INSSBuffer *t, DWORD *l) { *l = BUF(t)->max; return S_OK; }
static HRESULT WINAPI Buf_GetBuffer(INSSBuffer *t, BYTE **b) { *b = BUF(t)->data; return S_OK; }
static HRESULT WINAPI Buf_GetBufferAndLength(INSSBuffer *t, BYTE **b, DWORD *l) { *b = BUF(t)->data; *l = BUF(t)->len; return S_OK; }
static const INSSBufferVtbl buf_vtbl = {Buf_QI, Buf_AddRef, Buf_Release, Buf_GetLength, Buf_SetLength, Buf_GetMaxLength, Buf_GetBuffer, Buf_GetBufferAndLength};
static INSSBuffer *new_buf(BYTE *data, DWORD len) /* takes ownership of data */
{
    Buf *b = calloc(1, sizeof *b);
    b->iface.lpVtbl = &buf_vtbl; b->ref = 1; b->data = data; b->len = b->max = len;
    return &b->iface;
}

/* ---------- media props object ---------- */
typedef struct { IWMOutputMediaProps iface; LONG ref; WM_MEDIA_TYPE mt; BYTE fmt[256]; } Props;
#define PROPS(p) ((Props *)(p))
static HRESULT WINAPI Pr_QI(IWMOutputMediaProps *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown) || IsEqualGUID(r, &G_IWMMediaProps) || IsEqualGUID(r, &G_IWMOutputMediaProps)) {
        *o = t; InterlockedIncrement(&PROPS(t)->ref); return S_OK;
    }
    logmsg("Props QI %s -> E_NOINTERFACE", guidstr(r));
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI Pr_AddRef(IWMOutputMediaProps *t) { return InterlockedIncrement(&PROPS(t)->ref); }
static ULONG WINAPI Pr_Release(IWMOutputMediaProps *t)
{
    LONG n = InterlockedDecrement(&PROPS(t)->ref);
    if (!n) free(t);
    return n;
}
static HRESULT WINAPI Pr_GetType(IWMOutputMediaProps *t, GUID *g) { *g = PROPS(t)->mt.majortype; return S_OK; }
static HRESULT WINAPI Pr_GetMediaType(IWMOutputMediaProps *t, WM_MEDIA_TYPE *pt, DWORD *pcb)
{
    Props *p = PROPS(t);
    DWORD need = sizeof(WM_MEDIA_TYPE) + p->mt.cbFormat;
    if (!pcb) return E_INVALIDARG;
    if (!pt) { *pcb = need; return S_OK; }
    if (*pcb < need) { *pcb = need; return NS_E_SDK_BUFFERTOOSMALL; }
    *pt = p->mt;
    pt->pUnk = NULL;
    pt->pbFormat = (BYTE *)(pt + 1);
    memcpy(pt->pbFormat, p->fmt, p->mt.cbFormat);
    *pcb = need;
    return S_OK;
}
static HRESULT WINAPI Pr_SetMediaType(IWMOutputMediaProps *t, WM_MEDIA_TYPE *pt)
{
    Props *p = PROPS(t);
    if (!pt || pt->cbFormat > sizeof p->fmt) return E_INVALIDARG;
    p->mt = *pt; p->mt.pUnk = NULL;
    memcpy(p->fmt, pt->pbFormat, pt->cbFormat);
    p->mt.pbFormat = p->fmt;
    return S_OK;
}
static HRESULT WINAPI Pr_Name(IWMOutputMediaProps *t, WCHAR *n, WORD *c)
{
    if (!c) return E_INVALIDARG;
    if (!n) { *c = 1; return S_OK; }
    if (*c < 1) return NS_E_SDK_BUFFERTOOSMALL;
    n[0] = 0; *c = 1; return S_OK;
}
static const IWMOutputMediaPropsVtbl props_vtbl = {Pr_QI, Pr_AddRef, Pr_Release, Pr_GetType, Pr_GetMediaType, Pr_SetMediaType, Pr_Name, Pr_Name};

/* ---------- reader ---------- */
typedef struct {
    int kind; /* 0 audio, 1 video */
    int sidx; WORD snum;
    AVStream *st;
    AVCodecContext *dec;
    int selected;
    int vfmt;                 /* video: index in vfmts */
    int arate, ach;           /* audio output params */
    struct SwsContext *sws;
    struct SwrContext *swr;
    Sample *qh, *qt;
    int ended;
    QWORD lastts;
    DWORD maxsize;
    struct { WCHAR name[40]; DWORD v; } settings[8]; int nset;
} Out;

typedef struct {
    IWMSyncReader sr;
    IWMHeaderInfo3 hi;
    LONG ref;
    CRITICAL_SECTION cs;
    IStream *stream;
    AVFormatContext *fc;
    AVIOContext *avio;
    AVPacket *pkt;
    AVFrame *frame;
    Out outs[4];
    int nouts;
    int eof;
    QWORD rstart; LONGLONG rdur; QWORD rend; /* rend 0 = none */
    int disc;
    long ncalls;
} Reader;
#define RD(p) ((Reader *)(p))
#define RD_HI(p) ((Reader *)((BYTE *)(p) - offsetof(Reader, hi)))

static void q_clear(Out *o)
{
    while (o->qh) { Sample *s = o->qh; o->qh = s->next; free(s->data); free(s); }
    o->qt = NULL;
}
static void q_push(Out *o, Sample *s)
{
    s->next = NULL;
    if (o->qt) o->qt->next = s; else o->qh = s;
    o->qt = s;
}

/* IStream <-> AVIO */
static int io_read(void *op, uint8_t *buf, int size)
{
    ULONG n = 0;
    HRESULT hr = IStream_Read((IStream *)op, buf, size, &n);
    if (FAILED(hr)) return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    return (int)n;
}
static int64_t io_seek(void *op, int64_t off, int whence)
{
    IStream *s = op;
    LARGE_INTEGER li; ULARGE_INTEGER np;
    if (whence & AVSEEK_SIZE) {
        STATSTG st;
        if (FAILED(IStream_Stat(s, &st, STATFLAG_NONAME))) return -1;
        return (int64_t)st.cbSize.QuadPart;
    }
    li.QuadPart = off;
    whence &= ~AVSEEK_FORCE;
    if (FAILED(IStream_Seek(s, li, whence == SEEK_SET ? STREAM_SEEK_SET : whence == SEEK_CUR ? STREAM_SEEK_CUR : STREAM_SEEK_END, &np)))
        return -1;
    return (int64_t)np.QuadPart;
}

static QWORD to100ns(int64_t v, AVRational tb)
{
    if (v == AV_NOPTS_VALUE) return 0;
    return (QWORD)av_rescale_q(v, tb, (AVRational){1, 10000000});
}

static void build_mt(Out *o, int fi, Props *p)
{
    memset(p, 0, sizeof *p);
    p->iface.lpVtbl = &props_vtbl; p->ref = 1;
    AVCodecParameters *cp = o->st->codecpar;
    if (o->kind == 1) {
        const VFmt *f = &vfmts[fi];
        int w = cp->width, h = cp->height;
        VIH *v = (VIH *)p->fmt;
        DWORD size = (f->pix == AV_PIX_FMT_YUV420P) ? (DWORD)(w * h * 3 / 2) : (DWORD)(((w * f->bpp + 31) / 32) * 4) * h;
        v->rcSource = (RC){0, 0, w, h}; v->rcTarget = v->rcSource;
        AVRational fr = o->st->avg_frame_rate.num ? o->st->avg_frame_rate : o->st->r_frame_rate.num ? o->st->r_frame_rate : (AVRational){25, 1};
        v->AvgTimePerFrame = av_rescale(10000000, fr.den, fr.num);
        v->dwBitRate = (DWORD)cp->bit_rate;
        v->bmi.biSize = sizeof(BITMAPINFOHEADER); v->bmi.biWidth = w; v->bmi.biHeight = h;
        v->bmi.biPlanes = 1; v->bmi.biBitCount = f->bpp; v->bmi.biCompression = f->comp; v->bmi.biSizeImage = size;
        p->mt.majortype = G_Video; p->mt.subtype = *f->sub; p->mt.bFixedSizeSamples = TRUE; p->mt.bTemporalCompression = FALSE;
        p->mt.lSampleSize = size; p->mt.formattype = G_FmtVideo; p->mt.cbFormat = sizeof(VIH);
    } else {
        WAVEFORMATEX *w = (WAVEFORMATEX *)p->fmt;
        w->wFormatTag = WAVE_FORMAT_PCM; w->nChannels = o->ach; w->nSamplesPerSec = o->arate; w->wBitsPerSample = 16;
        w->nBlockAlign = w->nChannels * 2; w->nAvgBytesPerSec = w->nSamplesPerSec * w->nBlockAlign; w->cbSize = 0;
        p->mt.majortype = G_Audio; p->mt.subtype = G_PCM; p->mt.bFixedSizeSamples = TRUE; p->mt.bTemporalCompression = FALSE;
        p->mt.lSampleSize = w->nBlockAlign; p->mt.formattype = G_FmtWave; p->mt.cbFormat = sizeof(WAVEFORMATEX);
    }
    p->mt.pbFormat = p->fmt;
}

static void close_reader(Reader *r)
{
    int i;
    for (i = 0; i < r->nouts; i++) {
        Out *o = &r->outs[i];
        q_clear(o);
        if (o->dec) avcodec_free_context(&o->dec);
        if (o->sws) sws_freeContext(o->sws);
        if (o->swr) swr_free(&o->swr);
    }
    memset(r->outs, 0, sizeof r->outs);
    r->nouts = 0;
    if (r->fc) avformat_close_input(&r->fc);
    if (r->avio) { av_freep(&r->avio->buffer); avio_context_free(&r->avio); }
    if (r->pkt) av_packet_free(&r->pkt);
    if (r->frame) av_frame_free(&r->frame);
    if (r->stream) { IStream_Release(r->stream); r->stream = NULL; }
}

static int setup_outputs(Reader *r)
{
    int i, k;
    for (k = 0; k < 2; k++) { /* audio first, then video */
        for (i = 0; i < (int)r->fc->nb_streams && r->nouts < 4; i++) {
            AVStream *st = r->fc->streams[i];
            int want = k == 0 ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
            if (st->codecpar->codec_type != want) continue;
            const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
            if (!c) { logmsg("no decoder for stream %d codec_id %d", i, st->codecpar->codec_id); continue; }
            Out *o = &r->outs[r->nouts];
            o->kind = k; o->sidx = i; o->st = st; o->selected = 1;
            o->snum = st->id ? (WORD)st->id : (WORD)(i + 1);
            o->dec = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(o->dec, st->codecpar);
            o->dec->thread_count = 1;
            if (avcodec_open2(o->dec, c, NULL) < 0) { logmsg("avcodec_open2 failed stream %d", i); avcodec_free_context(&o->dec); continue; }
            if (k == 0) {
                o->arate = st->codecpar->sample_rate; o->ach = st->codecpar->ch_layout.nb_channels;
                if (o->ach < 1) o->ach = 2;
                o->maxsize = 65536 * o->ach;
            } else {
                o->vfmt = 0;
                o->maxsize = st->codecpar->width * st->codecpar->height * 4;
            }
            logmsg("output %d: kind=%d stream idx=%d num=%u codec=%s %dx%d rate=%d ch=%d", r->nouts, k, i, o->snum, c->name,
                   st->codecpar->width, st->codecpar->height, st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels);
            r->nouts++;
        }
    }
    return r->nouts ? 0 : -1;
}

static HRESULT open_stream(Reader *r, IStream *s, const char *fname)
{
    int ret;
    close_reader(r);
    r->eof = 0; r->rstart = 0; r->rdur = 0; r->rend = 0; r->disc = 1;
    r->fc = avformat_alloc_context();
    if (s) {
        uint8_t *buf = av_malloc(65536);
        IStream_AddRef(s); r->stream = s;
        r->avio = avio_alloc_context(buf, 65536, 0, s, io_read, NULL, io_seek);
        r->fc->pb = r->avio;
        r->fc->flags |= AVFMT_FLAG_CUSTOM_IO;
        ret = avformat_open_input(&r->fc, NULL, NULL, NULL);
    } else {
        ret = avformat_open_input(&r->fc, fname, NULL, NULL);
    }
    if (ret < 0) { logmsg("avformat_open_input failed %d", ret); r->fc = NULL; return NS_E_FILE_OPEN_FAILED; }
    if (avformat_find_stream_info(r->fc, NULL) < 0) logmsg("find_stream_info failed");
    if (setup_outputs(r) < 0) return NS_E_INVALID_DATA;
    r->pkt = av_packet_alloc(); r->frame = av_frame_alloc();
    logmsg("opened: duration=%lld us nouts=%d", (long long)r->fc->duration, r->nouts);
    return S_OK;
}

/* decode helpers */
static void push_video(Reader *r, Out *o, AVFrame *f)
{
    const VFmt *vf = &vfmts[o->vfmt];
    int w = f->width, h = f->height;
    DWORD size = (vf->pix == AV_PIX_FMT_YUV420P) ? (DWORD)(w * h * 3 / 2) : (DWORD)(((w * vf->bpp + 31) / 32) * 4) * h;
    BYTE *dst = calloc(1, size + 16);
    uint8_t *dp[4] = {0}; int ds[4] = {0};
    o->sws = sws_getCachedContext(o->sws, w, h, f->format, w, h, vf->pix, SWS_BILINEAR, NULL, NULL, NULL);
    if (vf->pix == AV_PIX_FMT_YUV420P) {
        dp[0] = dst; ds[0] = w;
        dp[1] = dst + w * h; ds[1] = w / 2;
        dp[2] = dst + w * h + (w / 2) * (h / 2); ds[2] = w / 2;
        if (vf->yv) { uint8_t *t = dp[1]; dp[1] = dp[2]; dp[2] = t; }
    } else {
        int stride = ((w * vf->bpp + 31) / 32) * 4;
        if (vf->flip) { dp[0] = dst + (size_t)stride * (h - 1); ds[0] = -stride; }
        else { dp[0] = dst; ds[0] = stride; }
    }
    sws_scale(o->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dp, ds);
    int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    Sample *s = calloc(1, sizeof *s);
    s->data = dst; s->len = size;
    if (pts == AV_NOPTS_VALUE) s->ts = o->lastts;
    else s->ts = pts < 0 ? 0 : to100ns(pts, o->st->time_base);
    AVRational fr = o->st->avg_frame_rate.num ? o->st->avg_frame_rate : o->st->r_frame_rate.num ? o->st->r_frame_rate : (AVRational){25, 1};
    s->dur = f->duration ? to100ns(f->duration, o->st->time_base) : av_rescale(10000000, fr.den, fr.num);
    s->flags = (f->pict_type == AV_PICTURE_TYPE_I) ? WM_SF_CLEANPOINT : 0;
    o->lastts = s->ts + s->dur;
    if (r->rend && s->ts >= r->rend) { o->ended = 1; free(s->data); free(s); return; }
    if (r->rstart && s->ts + s->dur <= r->rstart) { free(s->data); free(s); return; }
    q_push(o, s);
}
static void push_audio(Reader *r, Out *o, AVFrame *f)
{
    int n = f->nb_samples;
    int outn = (int)av_rescale_rnd(swr_get_delay(o->swr, f->sample_rate) + n, o->arate, f->sample_rate, AV_ROUND_UP) + 32;
    BYTE *dst = malloc((size_t)outn * o->ach * 2);
    uint8_t *dp[1] = {dst};
    int got = swr_convert(o->swr, dp, outn, (const uint8_t **)f->extended_data, n);
    if (got <= 0) { free(dst); return; }
    Sample *s = calloc(1, sizeof *s);
    s->data = dst; s->len = got * o->ach * 2;
    int64_t pts = f->pts;
    s->ts = pts == AV_NOPTS_VALUE ? o->lastts : (pts < 0 ? 0 : to100ns(pts, o->st->time_base));
    s->dur = (QWORD)got * 10000000ULL / o->arate;
    s->flags = WM_SF_CLEANPOINT;
    o->lastts = s->ts + s->dur;
    if (r->rend && s->ts >= r->rend) { o->ended = 1; free(s->data); free(s); return; }
    if (r->rstart && s->ts + s->dur <= r->rstart) { free(s->data); free(s); return; }
    q_push(o, s);
}
static void drain(Reader *r, Out *o)
{
    for (;;) {
        int ret = avcodec_receive_frame(o->dec, r->frame);
        if (ret < 0) break;
        if (o->kind == 1) push_video(r, o, r->frame);
        else {
            if (!o->swr) {
                AVChannelLayout ol;
                av_channel_layout_default(&ol, o->ach);
                swr_alloc_set_opts2(&o->swr, &ol, AV_SAMPLE_FMT_S16, o->arate, &r->frame->ch_layout, r->frame->format, r->frame->sample_rate, 0, NULL);
                swr_init(o->swr);
            }
            push_audio(r, o, r->frame);
        }
        av_frame_unref(r->frame);
    }
}
static void read_more(Reader *r)
{
    int i;
    if (r->eof) return;
    int ret = av_read_frame(r->fc, r->pkt);
    if (ret < 0) {
        r->eof = 1;
        for (i = 0; i < r->nouts; i++) { avcodec_send_packet(r->outs[i].dec, NULL); drain(r, &r->outs[i]); }
        return;
    }
    for (i = 0; i < r->nouts; i++) {
        Out *o = &r->outs[i];
        if (o->sidx != r->pkt->stream_index) continue;
        if (o->selected && !o->ended) {
            avcodec_send_packet(o->dec, r->pkt);
            drain(r, o);
        }
        break;
    }
    av_packet_unref(r->pkt);
}
static int out_done(Reader *r, Out *o) { return o->ended || r->eof; }

/* ---------- IWMSyncReader methods ---------- */
static HRESULT WINAPI SR_QI(IWMSyncReader *t, REFIID id, void **o)
{
    Reader *r = RD(t);
    if (IsEqualGUID(id, &G_IUnknown) || IsEqualGUID(id, &G_IWMSyncReader)) *o = &r->sr;
    else if (IsEqualGUID(id, &G_IWMHeaderInfo) || IsEqualGUID(id, &G_IWMHeaderInfo2) || IsEqualGUID(id, &G_IWMHeaderInfo3)) *o = &r->hi;
    else { logmsg("Reader QI %s -> E_NOINTERFACE", guidstr(id)); *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&r->ref);
    logmsg("Reader QI %s ok", guidstr(id));
    return S_OK;
}
static ULONG WINAPI SR_AddRef(IWMSyncReader *t) { return InterlockedIncrement(&RD(t)->ref); }
static ULONG WINAPI SR_Release(IWMSyncReader *t)
{
    Reader *r = RD(t);
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) { logmsg("Reader destroyed"); close_reader(r); DeleteCriticalSection(&r->cs); free(r); }
    return n;
}
static HRESULT WINAPI SR_Open(IWMSyncReader *t, const WCHAR *fn)
{
    char u[1024];
    WideCharToMultiByte(CP_UTF8, 0, fn, -1, u, sizeof u, NULL, NULL);
    logmsg("Open(%s)", u);
    EnterCriticalSection(&RD(t)->cs);
    HRESULT hr = open_stream(RD(t), NULL, u);
    LeaveCriticalSection(&RD(t)->cs);
    return hr;
}
static HRESULT WINAPI SR_OpenStream(IWMSyncReader *t, IStream *s)
{
    logmsg("OpenStream(%p)", s);
    EnterCriticalSection(&RD(t)->cs);
    HRESULT hr = open_stream(RD(t), s, NULL);
    LeaveCriticalSection(&RD(t)->cs);
    logmsg("OpenStream -> %08lx", hr);
    return hr;
}
static HRESULT WINAPI SR_Close(IWMSyncReader *t)
{
    logmsg("Close");
    EnterCriticalSection(&RD(t)->cs);
    close_reader(RD(t));
    LeaveCriticalSection(&RD(t)->cs);
    return S_OK;
}
static HRESULT WINAPI SR_SetRange(IWMSyncReader *t, QWORD start, LONGLONG dur)
{
    Reader *r = RD(t);
    int i;
    logmsg("SetRange(start=%llu dur=%lld)", (unsigned long long)start, (long long)dur);
    EnterCriticalSection(&r->cs);
    if (!r->fc) { LeaveCriticalSection(&r->cs); return NS_E_INVALID_REQUEST; }
    for (i = 0; i < r->nouts; i++) { q_clear(&r->outs[i]); r->outs[i].ended = 0; avcodec_flush_buffers(r->outs[i].dec); r->outs[i].lastts = start; }
    r->rstart = start; r->rdur = dur;
    r->rend = (dur > 0) ? start + (QWORD)dur : 0;
    r->eof = 0; r->disc = 1;
    int64_t us = (int64_t)(start / 10);
    int ret = av_seek_frame(r->fc, -1, us, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        logmsg("av_seek_frame failed %d, trying avformat_seek_file", ret);
        ret = avformat_seek_file(r->fc, -1, INT64_MIN, us, us, 0);
        logmsg("avformat_seek_file -> %d", ret);
    }
    LeaveCriticalSection(&r->cs);
    return S_OK;
}
static HRESULT WINAPI SR_SetRangeByFrame(IWMSyncReader *t, WORD s, QWORD f, LONGLONG c)
{
    Reader *r = RD(t);
    logmsg("SetRangeByFrame(stream=%u frame=%llu cnt=%lld)", s, (unsigned long long)f, (long long)c);
    for (int i = 0; i < r->nouts; i++) if (r->outs[i].snum == s && r->outs[i].kind == 1) {
        AVRational fr = r->outs[i].st->avg_frame_rate.num ? r->outs[i].st->avg_frame_rate : (AVRational){25, 1};
        QWORD st = (QWORD)av_rescale(f, 10000000LL * fr.den, fr.num);
        LONGLONG d = c > 0 ? (LONGLONG)av_rescale(c, 10000000LL * fr.den, fr.num) : 0;
        return SR_SetRange(t, st, d);
    }
    return NS_E_INVALID_REQUEST;
}
static HRESULT WINAPI SR_GetNextSample(IWMSyncReader *t, WORD snum, INSSBuffer **ppS, QWORD *pts, QWORD *pdur, DWORD *pflags, DWORD *pout, WORD *pstream)
{
    Reader *r = RD(t);
    int i, best = -1;
    EnterCriticalSection(&r->cs);
    r->ncalls++;
    if (!r->fc) { LeaveCriticalSection(&r->cs); return NS_E_INVALID_REQUEST; }
    if (snum) {
        for (i = 0; i < r->nouts; i++) if (r->outs[i].snum == snum) best = i;
        if (best < 0) { LeaveCriticalSection(&r->cs); logmsg("GetNextSample bad stream %u", snum); return NS_E_INVALID_REQUEST; }
        while (!r->outs[best].qh && !out_done(r, &r->outs[best])) read_more(r);
    } else {
        for (;;) {
            int need = 0;
            for (i = 0; i < r->nouts; i++) {
                Out *o = &r->outs[i];
                if (o->selected && !o->qh && !out_done(r, o)) need = 1;
            }
            if (!need) break;
            read_more(r);
        }
        for (i = 0; i < r->nouts; i++) {
            Out *o = &r->outs[i];
            if (o->selected && o->qh && (best < 0 || o->qh->ts < r->outs[best].qh->ts)) best = i;
        }
    }
    if (best < 0 || !r->outs[best].qh) {
        long nc = r->ncalls;
        LeaveCriticalSection(&r->cs);
        logmsg("GetNextSample(%u) -> NO_MORE_SAMPLES (call #%ld)", snum, nc);
        return NS_E_NO_MORE_SAMPLES;
    }
    Out *o = &r->outs[best];
    Sample *s = o->qh; o->qh = s->next; if (!o->qh) o->qt = NULL;
    if (r->disc) { s->flags |= WM_SF_DISCONTINUITY; r->disc = 0; }
    if (ppS) *ppS = new_buf(s->data, s->len);
    else free(s->data);
    if (pts) *pts = s->ts;
    if (pdur) *pdur = s->dur;
    if (pflags) *pflags = s->flags;
    if (pout) *pout = best;
    if (pstream) *pstream = o->snum;
    if (r->ncalls <= 12 || r->ncalls % 200 == 0)
        logmsg("GetNextSample(%u) #%ld -> out=%d len=%lu ts=%llu dur=%llu flags=%lx", snum, r->ncalls, best, s->len, (unsigned long long)s->ts, (unsigned long long)s->dur, s->flags);
    free(s);
    LeaveCriticalSection(&r->cs);
    return S_OK;
}
static HRESULT WINAPI SR_SetStreamsSelected(IWMSyncReader *t, WORD c, WORD *nums, WMT_STREAM_SELECTION *sel)
{
    Reader *r = RD(t);
    logmsg("SetStreamsSelected(%u)", c);
    for (int k = 0; k < c; k++) {
        logmsg("   stream %u sel=%d", nums[k], sel[k]);
        for (int i = 0; i < r->nouts; i++) if (r->outs[i].snum == nums[k]) r->outs[i].selected = sel[k] != WMT_OFF;
    }
    return S_OK;
}
static HRESULT WINAPI SR_GetStreamSelected(IWMSyncReader *t, WORD n, WMT_STREAM_SELECTION *sel)
{
    Reader *r = RD(t);
    for (int i = 0; i < r->nouts; i++) if (r->outs[i].snum == n) { *sel = r->outs[i].selected ? WMT_ON : WMT_OFF; return S_OK; }
    return NS_E_INVALID_REQUEST;
}
static HRESULT WINAPI SR_SetReadStreamSamples(IWMSyncReader *t, WORD n, WINBOOL comp)
{
    logmsg("SetReadStreamSamples(%u, compressed=%d)", n, comp);
    return comp ? E_NOTIMPL : S_OK;
}
static HRESULT WINAPI SR_GetReadStreamSamples(IWMSyncReader *t, WORD n, WINBOOL *comp) { *comp = FALSE; return S_OK; }
static HRESULT WINAPI SR_GetOutputSetting(IWMSyncReader *t, DWORD n, LPCWSTR name, WMT_ATTR_DATATYPE *type, BYTE *val, WORD *len)
{
    Reader *r = RD(t);
    char u[64];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
    logmsg("GetOutputSetting(%lu, %s)", n, u);
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    Out *o = &r->outs[n];
    *type = WMT_TYPE_BOOL;
    if (!val) { *len = 4; return S_OK; }
    *(DWORD *)val = 0; *len = 4;
    for (int i = 0; i < o->nset; i++) if (!lstrcmpW(o->settings[i].name, name)) *(DWORD *)val = o->settings[i].v;
    return S_OK;
}
static HRESULT WINAPI SR_SetOutputSetting(IWMSyncReader *t, DWORD n, LPCWSTR name, WMT_ATTR_DATATYPE type, const BYTE *val, WORD len)
{
    Reader *r = RD(t);
    char u[64];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
    logmsg("SetOutputSetting(%lu, %s, type=%d, len=%u, val=%lu)", n, u, type, len, (val && len >= 4) ? *(const DWORD *)val : 0);
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    Out *o = &r->outs[n];
    if (o->nset < 8 && val && len >= 4) {
        lstrcpynW(o->settings[o->nset].name, name, 40);
        o->settings[o->nset].v = *(const DWORD *)val;
        o->nset++;
    }
    return S_OK;
}
static HRESULT WINAPI SR_GetOutputCount(IWMSyncReader *t, DWORD *c) { *c = RD(t)->nouts; logmsg("GetOutputCount -> %d", RD(t)->nouts); return S_OK; }
static HRESULT make_props(Reader *r, DWORD n, int fi, IWMOutputMediaProps **pp)
{
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    Props *p = malloc(sizeof *p);
    build_mt(&r->outs[n], fi, p);
    *pp = &p->iface;
    return S_OK;
}
static HRESULT WINAPI SR_GetOutputProps(IWMSyncReader *t, DWORD n, IWMOutputMediaProps **pp)
{
    Reader *r = RD(t);
    logmsg("GetOutputProps(%lu)", n);
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    return make_props(r, n, r->outs[n].vfmt, pp);
}
static HRESULT WINAPI SR_SetOutputProps(IWMSyncReader *t, DWORD n, IWMOutputMediaProps *p)
{
    Reader *r = RD(t);
    BYTE buf[1024];
    DWORD cb = sizeof buf;
    WM_MEDIA_TYPE *mt = (WM_MEDIA_TYPE *)buf;
    if (n >= (DWORD)r->nouts || !p) return E_INVALIDARG;
    HRESULT hr = IWMOutputMediaProps_GetMediaType(p, mt, &cb);
    if (FAILED(hr)) return hr;
    Out *o = &r->outs[n];
    logmsg("SetOutputProps(%lu): major=%s sub=%s fmt=%s cb=%lu", n, guidstr(&mt->majortype), guidstr(&mt->subtype), guidstr(&mt->formattype), mt->cbFormat);
    if (o->kind == 1) {
        int i, pick = -1;
        for (i = 0; i < NVF; i++) if (IsEqualGUID(&mt->subtype, vfmts[i].sub)) { pick = i; break; }
        if (pick < 0 && mt->cbFormat >= sizeof(VIH)) {
            VIH *v = (VIH *)mt->pbFormat;
            logmsg("   unknown subtype; bmi comp=%08lx bpp=%u w=%ld h=%ld", v->bmi.biCompression, v->bmi.biBitCount, v->bmi.biWidth, v->bmi.biHeight);
            for (i = 0; i < NVF; i++) if (vfmts[i].comp == v->bmi.biCompression && vfmts[i].bpp == v->bmi.biBitCount) { pick = i; break; }
            if (pick < 0 && v->bmi.biCompression == BI_RGB) pick = v->bmi.biBitCount == 24 ? 1 : 0;
        }
        if (pick < 0) { logmsg("   -> unsupported video format"); return NS_E_INVALID_OUTPUT_FORMAT; }
        o->vfmt = pick;
        logmsg("   video output set to %s", vfmts[pick].name);
    } else if (mt->cbFormat >= sizeof(WAVEFORMATEX)) {
        WAVEFORMATEX *w = (WAVEFORMATEX *)mt->pbFormat;
        logmsg("   audio: tag=%u ch=%u rate=%lu bits=%u", w->wFormatTag, w->nChannels, w->nSamplesPerSec, w->wBitsPerSample);
        if (w->wFormatTag != WAVE_FORMAT_PCM || w->wBitsPerSample != 16) return NS_E_INVALID_OUTPUT_FORMAT;
        if (w->nChannels != o->ach || w->nSamplesPerSec != (DWORD)o->arate) {
            o->ach = w->nChannels; o->arate = w->nSamplesPerSec;
            if (o->swr) swr_free(&o->swr);
        }
    }
    return S_OK;
}
static HRESULT WINAPI SR_GetOutputFormatCount(IWMSyncReader *t, DWORD n, DWORD *c)
{
    Reader *r = RD(t);
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    *c = r->outs[n].kind == 1 ? NVF : 1;
    logmsg("GetOutputFormatCount(%lu) -> %lu", n, *c);
    return S_OK;
}
static HRESULT WINAPI SR_GetOutputFormat(IWMSyncReader *t, DWORD n, DWORD f, IWMOutputMediaProps **pp)
{
    Reader *r = RD(t);
    logmsg("GetOutputFormat(%lu, %lu)", n, f);
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    if (f >= (r->outs[n].kind == 1 ? (DWORD)NVF : 1)) return E_INVALIDARG;
    return make_props(r, n, (int)f, pp);
}
static HRESULT WINAPI SR_GetOutputNumberForStream(IWMSyncReader *t, WORD s, DWORD *n)
{
    Reader *r = RD(t);
    for (int i = 0; i < r->nouts; i++) if (r->outs[i].snum == s) { *n = i; return S_OK; }
    return NS_E_INVALID_REQUEST;
}
static HRESULT WINAPI SR_GetStreamNumberForOutput(IWMSyncReader *t, DWORD n, WORD *s)
{
    Reader *r = RD(t);
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    *s = r->outs[n].snum; return S_OK;
}
static HRESULT WINAPI SR_GetMaxOutputSampleSize(IWMSyncReader *t, DWORD n, DWORD *m)
{
    Reader *r = RD(t);
    if (n >= (DWORD)r->nouts) return E_INVALIDARG;
    Out *o = &r->outs[n];
    if (o->kind == 1) { int w = o->st->codecpar->width, h = o->st->codecpar->height; *m = w * h * 4; }
    else *m = o->maxsize;
    logmsg("GetMaxOutputSampleSize(%lu) -> %lu", n, *m);
    return S_OK;
}
static HRESULT WINAPI SR_GetMaxStreamSampleSize(IWMSyncReader *t, WORD s, DWORD *m) { *m = 0x20000; return S_OK; }

static const IWMSyncReaderVtbl sr_vtbl = {
    SR_QI, SR_AddRef, SR_Release, SR_Open, SR_Close, SR_SetRange, SR_SetRangeByFrame, SR_GetNextSample,
    SR_SetStreamsSelected, SR_GetStreamSelected, SR_SetReadStreamSamples, SR_GetReadStreamSamples,
    SR_GetOutputSetting, SR_SetOutputSetting, SR_GetOutputCount, SR_GetOutputProps, SR_SetOutputProps,
    SR_GetOutputFormatCount, SR_GetOutputFormat, SR_GetOutputNumberForStream, SR_GetStreamNumberForOutput,
    SR_GetMaxOutputSampleSize, SR_GetMaxStreamSampleSize, SR_OpenStream};

/* ---------- IWMHeaderInfo3 ---------- */
static HRESULT WINAPI HI_QI(IWMHeaderInfo3 *t, REFIID id, void **o) { return SR_QI(&RD_HI(t)->sr, id, o); }
static ULONG WINAPI HI_AddRef(IWMHeaderInfo3 *t) { return SR_AddRef(&RD_HI(t)->sr); }
static ULONG WINAPI HI_Release(IWMHeaderInfo3 *t) { return SR_Release(&RD_HI(t)->sr); }
static HRESULT WINAPI HI_GetAttributeCount(IWMHeaderInfo3 *t, WORD s, WORD *c) { logmsg("HI GetAttributeCount(%u)", s); *c = 0; return S_OK; }
static HRESULT WINAPI HI_GetAttributeByIndex(IWMHeaderInfo3 *t, WORD i, WORD *s, WCHAR *n, WORD *nl, WMT_ATTR_DATATYPE *ty, BYTE *v, WORD *vl)
{ logmsg("HI GetAttributeByIndex(%u)", i); return E_INVALIDARG; }
static HRESULT WINAPI HI_GetAttributeByName(IWMHeaderInfo3 *t, WORD *s, LPCWSTR name, WMT_ATTR_DATATYPE *ty, BYTE *v, WORD *vl)
{
    Reader *r = RD_HI(t);
    char u[96];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
    logmsg("HI GetAttributeByName(stream=%u, %s)", s ? *s : 0, u);
    if (!r->fc) return NS_E_INVALID_REQUEST;
    if (!strcmp(u, "Duration") || !strcmp(u, "PlayDuration")) {
        *ty = WMT_TYPE_QWORD;
        if (!v) { *vl = 8; return S_OK; }
        if (*vl < 8) return NS_E_SDK_BUFFERTOOSMALL;
        *(QWORD *)v = (QWORD)r->fc->duration * 10; *vl = 8;
        return S_OK;
    }
    if (!strcmp(u, "Seekable") || !strcmp(u, "Stridable")) {
        *ty = WMT_TYPE_BOOL;
        if (!v) { *vl = 4; return S_OK; }
        if (*vl < 4) return NS_E_SDK_BUFFERTOOSMALL;
        *(DWORD *)v = 1; *vl = 4;
        return S_OK;
    }
    if (!strcmp(u, "Bitrate") || !strcmp(u, "MaxBitrate")) {
        *ty = WMT_TYPE_DWORD;
        if (!v) { *vl = 4; return S_OK; }
        *(DWORD *)v = (DWORD)r->fc->bit_rate; *vl = 4;
        return S_OK;
    }
    return E_NOTFOUND_ASF;
}
static HRESULT WINAPI HI_SetAttribute(IWMHeaderInfo3 *t, WORD s, LPCWSTR n, WMT_ATTR_DATATYPE ty, const BYTE *v, WORD l) { return E_NOTIMPL; }
static HRESULT WINAPI HI_GetMarkerCount(IWMHeaderInfo3 *t, WORD *c) { *c = 0; return S_OK; }
static HRESULT WINAPI HI_GetMarker(IWMHeaderInfo3 *t, WORD i, WCHAR *n, WORD *nl, QWORD *ti) { return E_INVALIDARG; }
static HRESULT WINAPI HI_AddMarker(IWMHeaderInfo3 *t, LPCWSTR n, QWORD ti) { return E_NOTIMPL; }
static HRESULT WINAPI HI_RemoveMarker(IWMHeaderInfo3 *t, WORD i) { return E_NOTIMPL; }
static HRESULT WINAPI HI_GetScriptCount(IWMHeaderInfo3 *t, WORD *c) { *c = 0; return S_OK; }
static HRESULT WINAPI HI_GetScript(IWMHeaderInfo3 *t, WORD i, WCHAR *ty, WORD *tl, WCHAR *cmd, WORD *cl, QWORD *ti) { return E_INVALIDARG; }
static HRESULT WINAPI HI_AddScript(IWMHeaderInfo3 *t, LPCWSTR ty, LPCWSTR cmd, QWORD ti) { return E_NOTIMPL; }
static HRESULT WINAPI HI_RemoveScript(IWMHeaderInfo3 *t, WORD i) { return E_NOTIMPL; }
static HRESULT WINAPI HI_GetCodecInfoCount(IWMHeaderInfo3 *t, DWORD *c) { *c = 0; return S_OK; }
static HRESULT WINAPI HI_GetCodecInfo(IWMHeaderInfo3 *t, DWORD i, WORD *nl, WCHAR *n, WORD *dl, WCHAR *d, WMT_CODEC_INFO_TYPE *ty, WORD *sl, BYTE *sz) { return E_INVALIDARG; }
static HRESULT WINAPI HI_GetAttributeCountEx(IWMHeaderInfo3 *t, WORD s, WORD *c) { logmsg("HI GetAttributeCountEx(%u)", s); *c = 0; return S_OK; }
static HRESULT WINAPI HI_GetAttributeIndices(IWMHeaderInfo3 *t, WORD s, LPCWSTR n, WORD *lang, WORD *idx, WORD *cnt) { logmsg("HI GetAttributeIndices"); *cnt = 0; return E_NOTFOUND_ASF; }
static HRESULT WINAPI HI_GetAttributeByIndexEx(IWMHeaderInfo3 *t, WORD s, WORD i, LPWSTR n, WORD *nl, WMT_ATTR_DATATYPE *ty, WORD *lang, BYTE *v, DWORD *vl) { logmsg("HI GetAttributeByIndexEx"); return E_INVALIDARG; }
static HRESULT WINAPI HI_ModifyAttribute(IWMHeaderInfo3 *t, WORD s, WORD i, WMT_ATTR_DATATYPE ty, WORD lang, const BYTE *v, DWORD vl) { return E_NOTIMPL; }
static HRESULT WINAPI HI_AddAttribute(IWMHeaderInfo3 *t, WORD s, LPCWSTR n, WORD *idx, WMT_ATTR_DATATYPE ty, WORD lang, const BYTE *v, DWORD vl) { return E_NOTIMPL; }
static HRESULT WINAPI HI_DeleteAttribute(IWMHeaderInfo3 *t, WORD s, WORD i) { return E_NOTIMPL; }
static HRESULT WINAPI HI_AddCodecInfo(IWMHeaderInfo3 *t, LPCWSTR n, LPCWSTR d, WMT_CODEC_INFO_TYPE ty, WORD sl, BYTE *sz) { return E_NOTIMPL; }
static const IWMHeaderInfo3Vtbl hi_vtbl = {
    HI_QI, HI_AddRef, HI_Release, HI_GetAttributeCount, HI_GetAttributeByIndex, HI_GetAttributeByName, HI_SetAttribute,
    HI_GetMarkerCount, HI_GetMarker, HI_AddMarker, HI_RemoveMarker, HI_GetScriptCount, HI_GetScript, HI_AddScript,
    HI_RemoveScript, HI_GetCodecInfoCount, HI_GetCodecInfo, HI_GetAttributeCountEx, HI_GetAttributeIndices,
    HI_GetAttributeByIndexEx, HI_ModifyAttribute, HI_AddAttribute, HI_DeleteAttribute, HI_AddCodecInfo};

/* ---------- exports ---------- */
HRESULT WINAPI WMCreateSyncReader(IUnknown *pUnkCert, DWORD dwRights, IWMSyncReader **ppReader)
{
    logmsg("WMCreateSyncReader(%p, %lu)", pUnkCert, dwRights);
    if (!ppReader) return E_POINTER;
    av_log_set_level(AV_LOG_QUIET);
    Reader *r = calloc(1, sizeof *r);
    r->sr.lpVtbl = &sr_vtbl; r->hi.lpVtbl = &hi_vtbl; r->ref = 1;
    InitializeCriticalSection(&r->cs);
    *ppReader = &r->sr;
    return S_OK;
}

HRESULT profile_manager_create(IWMProfileManager **pp);
HRESULT WINAPI WMCreateProfileManager(IWMProfileManager **pp)
{
    logmsg("WMCreateProfileManager");
    if (!pp) return E_POINTER;
    return profile_manager_create(pp);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, void *res) { return TRUE; }
