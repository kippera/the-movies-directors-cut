/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DirectShow source filter replacing the "WM ASF Reader" (CLSID_WMAsfReader) with an FFmpeg-backed one.
 * Outputs decoded RGB24 video (bottom-up DIB) and 16-bit PCM audio, so no WMV/WMA decoders or
 * colour converters are needed in the graph. */
#define COBJMACROS
#define STRSAFE_NO_DEPRECATE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <dshow.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

static const CLSID CLSID_WMAsfReader_ = {0x187463a0, 0x5bb7, 0x11d3, {0xac, 0xbe, 0x00, 0x80, 0xc7, 0x5e, 0x24, 0x6e}};
static const GUID G_TimeFmt = {0x7b785574, 0x8c82, 0x11cf, {0xbc, 0x0c, 0x00, 0xaa, 0x00, 0xac, 0x74, 0xf6}};
static const GUID G_MemAllocClsid = {0x1e651cc0, 0xb199, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
static const GUID G_RGB24 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID G_Video = {0x73646976, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_Audio = {0x73647561, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_PCM = {0x00000001, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_FmtVideo = {0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID G_FmtWave = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID G_IMiscFlags = {0x2dd74950, 0xa890, 0x11d1, {0xab, 0xe8, 0x00, 0xa0, 0xc9, 0x05, 0xf3, 0x75}};
static const GUID G_IUnknown_ = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

#define VFW_E_NOT_CONNECTED_ ((HRESULT)0x80040209)
#define VFW_E_ALREADY_CONNECTED_ ((HRESULT)0x80040204)
#define VFW_E_TYPE_NOT_ACCEPTED_ ((HRESULT)0x8004022A)
#define VFW_E_NOT_STOPPED_ ((HRESULT)0x80040225)
#define VFW_E_NOT_FOUND_ ((HRESULT)0x80040216)

/* ---------- logging ---------- */
static FILE *logf_;
static CRITICAL_SECTION logcs;
static LONG logready;
void logmsg(const char *fmt, ...)
{
    va_list ap;
    static LONG dbg = -1; /* logging is OFF unless the marker file C:\\asfreader.debug exists (the log grew to 25 MB per export session) */
    if (dbg < 0) dbg = (GetFileAttributesA("C:\\asfreader.debug") != INVALID_FILE_ATTRIBUTES);
    if (!dbg) return;
    if (!logf_) {
        logf_ = fopen("C:\\asfreader.log", "a");
        if (!logf_) return;
        InitializeCriticalSection(&logcs);
        fprintf(logf_, "---- asfreader loaded (pid %lu)\n", GetCurrentProcessId());
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

/* ---------- structures ---------- */
typedef struct Sm { struct Sm *next; BYTE *data; DWORD len; LONGLONG ts, dur; int eos; } Sm;
typedef struct Filter Filter;
typedef struct Pin {
    IPin pin;
    IMediaSeeking seek;
    Filter *f;
    int kind; /* 0 audio, 1 video */
    WCHAR name[32];
    IPin *peer; IMemInputPin *mip; IMemAllocator *alloc; ALLOCATOR_PROPERTIES ap;
    int sidx; AVStream *st; AVCodecContext *dec;
    struct SwsContext *sws; struct SwrContext *swr;
    int arate, ach, w, h;
    Sm *qh, *qt; int qn;
    HANDLE thread;
    int disc;
} Pin;

struct Filter {
    IBaseFilter bf;
    IFileSourceFilter fsf;
    IAMFilterMiscFlags mf;
    LONG ref;
    CRITICAL_SECTION lock, qlock;
    CONDITION_VARIABLE qcv;
    FILTER_STATE state;
    IFilterGraph *graph; WCHAR gname[128];
    IReferenceClock *clock;
    WCHAR *file;
    AVFormatContext *fc; AVPacket *pkt; AVFrame *frame;
    Pin pins[2]; Pin *order[2]; int npins;
    volatile LONG abort; HANDLE demux; int threads_on;
    LONGLONG segstart, stop, duration, curpos;
    int eof; DWORD lastseek;
};
#define F_BF(p) ((Filter *)(p))
#define F_FS(p) ((Filter *)((BYTE *)(p) - offsetof(Filter, fsf)))
#define F_MF(p) ((Filter *)((BYTE *)(p) - offsetof(Filter, mf)))
#define P_PIN(p) ((Pin *)(p))
#define P_SK(p) ((Pin *)((BYTE *)(p) - offsetof(Pin, seek)))

static LONGLONG to_rt(int64_t v, AVRational tb) { return v == AV_NOPTS_VALUE ? 0 : av_rescale_q(v, tb, (AVRational){1, 10000000}); }

/* ---------- queues ---------- */
static void q_push(Pin *p, Sm *s)
{
    Filter *f = p->f;
    s->next = NULL;
    EnterCriticalSection(&f->qlock);
    if (p->qt) p->qt->next = s; else p->qh = s;
    p->qt = s; p->qn++;
    WakeAllConditionVariable(&f->qcv);
    LeaveCriticalSection(&f->qlock);
}
static void q_clear(Pin *p)
{
    while (p->qh) { Sm *s = p->qh; p->qh = s->next; free(s->data); free(s); }
    p->qt = NULL; p->qn = 0;
}

/* ---------- media types ---------- */
typedef struct { RECT rcSource, rcTarget; DWORD dwBitRate, dwBitErrorRate; LONGLONG AvgTimePerFrame; BITMAPINFOHEADER bmi; } VIH_;

static void fill_mt(Pin *p, AM_MEDIA_TYPE *mt)
{
    memset(mt, 0, sizeof *mt);
    if (p->kind == 1) {
        VIH_ *v = CoTaskMemAlloc(sizeof *v);
        memset(v, 0, sizeof *v);
        int w = p->w, h = p->h;
        AVRational fr = p->st->avg_frame_rate.num ? p->st->avg_frame_rate : p->st->r_frame_rate.num ? p->st->r_frame_rate : (AVRational){25, 1};
        SetRect(&v->rcSource, 0, 0, w, h); v->rcTarget = v->rcSource;
        v->AvgTimePerFrame = av_rescale(10000000, fr.den, fr.num);
        v->dwBitRate = (DWORD)p->st->codecpar->bit_rate;
        v->bmi.biSize = sizeof(BITMAPINFOHEADER); v->bmi.biWidth = w; v->bmi.biHeight = h; v->bmi.biPlanes = 1;
        v->bmi.biBitCount = 24; v->bmi.biCompression = BI_RGB; v->bmi.biSizeImage = ((w * 24 + 31) / 32) * 4 * h;
        mt->majortype = G_Video; mt->subtype = G_RGB24; mt->bFixedSizeSamples = TRUE; mt->lSampleSize = v->bmi.biSizeImage;
        mt->formattype = G_FmtVideo; mt->cbFormat = sizeof *v; mt->pbFormat = (BYTE *)v;
    } else {
        WAVEFORMATEX *w = CoTaskMemAlloc(sizeof *w);
        memset(w, 0, sizeof *w);
        w->wFormatTag = WAVE_FORMAT_PCM; w->nChannels = p->ach; w->nSamplesPerSec = p->arate; w->wBitsPerSample = 16;
        w->nBlockAlign = w->nChannels * 2; w->nAvgBytesPerSec = w->nSamplesPerSec * w->nBlockAlign;
        mt->majortype = G_Audio; mt->subtype = G_PCM; mt->bFixedSizeSamples = TRUE; mt->lSampleSize = w->nBlockAlign;
        mt->formattype = G_FmtWave; mt->cbFormat = sizeof *w; mt->pbFormat = (BYTE *)w;
    }
}
static void free_mt(AM_MEDIA_TYPE *mt) { if (mt->pbFormat) CoTaskMemFree(mt->pbFormat); mt->pbFormat = NULL; mt->cbFormat = 0; }
static AM_MEDIA_TYPE *dup_mt(const AM_MEDIA_TYPE *s)
{
    AM_MEDIA_TYPE *d = CoTaskMemAlloc(sizeof *d);
    *d = *s; d->pUnk = NULL;
    if (s->cbFormat) { d->pbFormat = CoTaskMemAlloc(s->cbFormat); memcpy(d->pbFormat, s->pbFormat, s->cbFormat); }
    return d;
}
static int mt_ok(Pin *p, const AM_MEDIA_TYPE *m)
{
    if (!m) return 1;
    if (!IsEqualGUID(&m->majortype, p->kind ? &G_Video : &G_Audio) && !IsEqualGUID(&m->majortype, &GUID_NULL)) return 0;
    if (!IsEqualGUID(&m->subtype, &GUID_NULL) && !IsEqualGUID(&m->subtype, p->kind ? &G_RGB24 : &G_PCM)) return 0;
    if (!IsEqualGUID(&m->formattype, &GUID_NULL) && !IsEqualGUID(&m->formattype, p->kind ? &G_FmtVideo : &G_FmtWave)) return 0;
    return 1;
}

/* ---------- IEnumMediaTypes ---------- */
typedef struct { IEnumMediaTypes e; LONG ref; Pin *p; int idx; } EMT;
static HRESULT WINAPI EMT_QI(IEnumMediaTypes *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IEnumMediaTypes)) { *o = t; ((EMT *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI EMT_AddRef(IEnumMediaTypes *t) { return ++((EMT *)t)->ref; }
static ULONG WINAPI EMT_Release(IEnumMediaTypes *t) { EMT *e = (EMT *)t; if (!--e->ref) { free(e); return 0; } return e->ref; }
static HRESULT WINAPI EMT_Next(IEnumMediaTypes *t, ULONG n, AM_MEDIA_TYPE **mts, ULONG *got)
{
    EMT *e = (EMT *)t; ULONG k = 0;
    while (k < n && e->idx < 1) {
        AM_MEDIA_TYPE m; fill_mt(e->p, &m);
        mts[k++] = dup_mt(&m); free_mt(&m); e->idx++;
    }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI EMT_Skip(IEnumMediaTypes *t, ULONG n) { EMT *e = (EMT *)t; e->idx += n; return e->idx > 1 ? S_FALSE : S_OK; }
static HRESULT WINAPI EMT_Reset(IEnumMediaTypes *t) { ((EMT *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI EMT_Clone(IEnumMediaTypes *t, IEnumMediaTypes **o)
{
    EMT *e = (EMT *)t, *n = calloc(1, sizeof *n);
    *n = *e; n->ref = 1; *o = &n->e; return S_OK;
}
static const IEnumMediaTypesVtbl emt_vtbl = {EMT_QI, EMT_AddRef, EMT_Release, EMT_Next, EMT_Skip, EMT_Reset, EMT_Clone};

/* ---------- decoding ---------- */
static void push_video(Filter *f, Pin *p, AVFrame *fr)
{
    int w = fr->width, h = fr->height;
    int stride = ((w * 24 + 31) / 32) * 4;
    DWORD size = stride * h;
    BYTE *dst = calloc(1, size + 16);
    uint8_t *dp[4] = {dst + (size_t)stride * (h - 1)}; int ds[4] = {-stride};
    p->sws = sws_getCachedContext(p->sws, w, h, fr->format, w, h, AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
    sws_scale(p->sws, (const uint8_t *const *)fr->data, fr->linesize, 0, h, dp, ds);
    int64_t pts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
    Sm *s = calloc(1, sizeof *s);
    s->data = dst; s->len = size;
    s->ts = pts == AV_NOPTS_VALUE ? p->disc * 0 : to_rt(pts, p->st->time_base);
    if (s->ts < 0) s->ts = 0;
    AVRational fr_ = p->st->avg_frame_rate.num ? p->st->avg_frame_rate : p->st->r_frame_rate.num ? p->st->r_frame_rate : (AVRational){25, 1};
    s->dur = fr->duration ? to_rt(fr->duration, p->st->time_base) : av_rescale(10000000, fr_.den, fr_.num);
    if (f->segstart && s->ts + s->dur <= f->segstart) { free(s->data); free(s); return; }
    if (f->stop && s->ts >= f->stop) { free(s->data); free(s); return; }
    q_push(p, s);
}
#define ACHUNK 8192
static void push_audio(Filter *f, Pin *p, AVFrame *fr)
{
    int n = fr->nb_samples;
    int outn = (int)av_rescale_rnd(swr_get_delay(p->swr, fr->sample_rate) + n, p->arate, fr->sample_rate, AV_ROUND_UP) + 32;
    BYTE *dst = malloc((size_t)outn * p->ach * 2);
    uint8_t *dp[1] = {dst};
    int got = swr_convert(p->swr, dp, outn, (const uint8_t **)fr->extended_data, n);
    if (got <= 0) { free(dst); return; }
    LONGLONG ts = fr->pts == AV_NOPTS_VALUE ? 0 : to_rt(fr->pts, p->st->time_base);
    if (ts < 0) ts = 0;
    int off = 0;
    while (off < got) {
        int c = got - off > ACHUNK ? ACHUNK : got - off;
        Sm *s = calloc(1, sizeof *s);
        s->len = c * p->ach * 2; s->data = malloc(s->len);
        memcpy(s->data, dst + (size_t)off * p->ach * 2, s->len);
        s->ts = ts + (LONGLONG)off * 10000000LL / p->arate;
        s->dur = (LONGLONG)c * 10000000LL / p->arate;
        off += c;
        if ((f->segstart && s->ts + s->dur <= f->segstart) || (f->stop && s->ts >= f->stop)) { free(s->data); free(s); continue; }
        q_push(p, s);
    }
    free(dst);
}
static void drain(Filter *f, Pin *p)
{
    for (;;) {
        if (avcodec_receive_frame(p->dec, f->frame) < 0) break;
        if (p->kind == 1) push_video(f, p, f->frame);
        else {
            if (!p->swr) {
                AVChannelLayout ol;
                av_channel_layout_default(&ol, p->ach);
                swr_alloc_set_opts2(&p->swr, &ol, AV_SAMPLE_FMT_S16, p->arate, &f->frame->ch_layout, f->frame->format, f->frame->sample_rate, 0, NULL);
                swr_init(p->swr);
            }
            push_audio(f, p, f->frame);
        }
        av_frame_unref(f->frame);
    }
}
#define VCAP 60
#define ACAP 100
static DWORD WINAPI demux_proc(void *arg)
{
    Filter *f = arg;
    logmsg("demux thread start segstart=%lld", f->segstart);
    while (!f->abort) {
        int i, full = 0;
        EnterCriticalSection(&f->qlock);
        for (;;) {
            full = 0;
            for (i = 0; i < f->npins; i++) {
                Pin *p = f->order[i];
                if (p->peer && p->qn >= (p->kind ? VCAP : ACAP)) full = 1;
            }
            if (!full || f->abort) break;
            SleepConditionVariableCS(&f->qcv, &f->qlock, INFINITE);
        }
        LeaveCriticalSection(&f->qlock);
        if (f->abort) break;
        int ret = av_read_frame(f->fc, f->pkt);
        if (ret < 0) {
            logmsg("demux EOF (%d)", ret);
            for (i = 0; i < f->npins; i++) {
                Pin *p = f->order[i];
                if (!p->peer) continue;
                avcodec_send_packet(p->dec, NULL); drain(f, p);
                Sm *s = calloc(1, sizeof *s); s->eos = 1; q_push(p, s);
            }
            break;
        }
        for (i = 0; i < f->npins; i++) {
            Pin *p = f->order[i];
            if (p->sidx != f->pkt->stream_index) continue;
            if (p->peer) { avcodec_send_packet(p->dec, f->pkt); drain(f, p); }
            break;
        }
        av_packet_unref(f->pkt);
    }
    logmsg("demux thread exit");
    return 0;
}
static DWORD WINAPI pin_proc(void *arg)
{
    Pin *p = arg; Filter *f = p->f;
    long n = 0;
    logmsg("pin thread %d start", p->kind);
    for (;;) {
        Sm *s;
        EnterCriticalSection(&f->qlock);
        while (!p->qh && !f->abort) SleepConditionVariableCS(&f->qcv, &f->qlock, INFINITE);
        if (f->abort) { LeaveCriticalSection(&f->qlock); break; }
        s = p->qh; p->qh = s->next; if (!p->qh) p->qt = NULL; p->qn--;
        WakeAllConditionVariable(&f->qcv);
        LeaveCriticalSection(&f->qlock);
        if (s->eos) {
            logmsg("pin %d: EndOfStream", p->kind);
            free(s);
            IPin_EndOfStream(p->peer);
            break;
        }
        IMediaSample *ms = NULL;
        HRESULT hr = IMemAllocator_GetBuffer(p->alloc, &ms, NULL, NULL, 0);
        if (FAILED(hr)) { logmsg("pin %d: GetBuffer failed %08lx", p->kind, hr); free(s->data); free(s); break; }
        BYTE *buf; IMediaSample_GetPointer(ms, &buf);
        LONG cap = IMediaSample_GetSize(ms);
        DWORD len = s->len > (DWORD)cap ? (DWORD)cap : s->len;
        memcpy(buf, s->data, len);
        IMediaSample_SetActualDataLength(ms, len);
        REFERENCE_TIME a = s->ts - f->segstart, b = a + s->dur;
        if (a < 0) a = 0;
        IMediaSample_SetTime(ms, &a, &b);
        IMediaSample_SetSyncPoint(ms, TRUE);
        IMediaSample_SetDiscontinuity(ms, p->disc);
        p->disc = 0;
        if (p->kind == 1) f->curpos = s->ts;
        hr = IMemInputPin_Receive(p->mip, ms);
        IMediaSample_Release(ms);
        if (++n <= 5 || n % 200 == 0) logmsg("pin %d: sample #%ld ts=%lld len=%lu -> %08lx", p->kind, n, s->ts, len, hr);
        free(s->data); free(s);
        if (hr != S_OK) { logmsg("pin %d: Receive returned %08lx, stopping", p->kind, hr); break; }
    }
    logmsg("pin thread %d exit", p->kind);
    return 0;
}

/* ---------- thread control ---------- */
static void seek_ffmpeg(Filter *f, LONGLONG rt)
{
    int i;
    int ret = av_seek_frame(f->fc, -1, rt / 10, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) { ret = avformat_seek_file(f->fc, -1, INT64_MIN, rt / 10, rt / 10, 0); logmsg("fallback seek -> %d", ret); }
    for (i = 0; i < f->npins; i++) if (f->order[i]->dec) avcodec_flush_buffers(f->order[i]->dec);
}
static void start_threads(Filter *f)
{
    int i;
    if (f->threads_on || !f->fc) return;
    logmsg("start_threads segstart=%lld", f->segstart);
    f->abort = 0;
    for (i = 0; i < f->npins; i++) {
        Pin *p = f->order[i];
        q_clear(p);
        if (!p->peer) continue;
        IMemAllocator_Commit(p->alloc);
        IPin_NewSegment(p->peer, f->segstart, f->stop, 1.0);
        p->disc = 1;
    }
    seek_ffmpeg(f, f->segstart);
    f->demux = CreateThread(NULL, 0, demux_proc, f, 0, NULL);
    for (i = 0; i < f->npins; i++) {
        Pin *p = f->order[i];
        if (p->peer) p->thread = CreateThread(NULL, 0, pin_proc, p, 0, NULL);
    }
    f->threads_on = 1;
}
static void stop_threads(Filter *f, int flush)
{
    int i;
    if (!f->threads_on) return;
    logmsg("stop_threads flush=%d", flush);
    if (flush) for (i = 0; i < f->npins; i++) if (f->order[i]->peer) IPin_BeginFlush(f->order[i]->peer);
    InterlockedExchange(&f->abort, 1);
    for (i = 0; i < f->npins; i++) if (f->order[i]->peer) IMemAllocator_Decommit(f->order[i]->alloc);
    EnterCriticalSection(&f->qlock);
    WakeAllConditionVariable(&f->qcv);
    LeaveCriticalSection(&f->qlock);
    if (f->demux) { WaitForSingleObject(f->demux, INFINITE); CloseHandle(f->demux); f->demux = NULL; }
    for (i = 0; i < f->npins; i++) {
        Pin *p = f->order[i];
        if (p->thread) { WaitForSingleObject(p->thread, INFINITE); CloseHandle(p->thread); p->thread = NULL; }
        q_clear(p);
    }
    if (flush) for (i = 0; i < f->npins; i++) if (f->order[i]->peer) IPin_EndFlush(f->order[i]->peer);
    f->threads_on = 0;
    logmsg("stop_threads done");
}
static HRESULT do_seek(Filter *f, LONGLONG start, LONGLONG stop, int setstop)
{
    int was;
    EnterCriticalSection(&f->lock);
    was = f->threads_on;
    logmsg("do_seek start=%lld stop=%lld was_running=%d", start, setstop ? stop : -1, was);
    if (was) stop_threads(f, 1);
    if (start < 0) start = 0;
    if (start > f->duration) start = f->duration;
    f->segstart = start;
    f->curpos = start;
    if (setstop) f->stop = stop >= f->duration ? 0 : stop;
    if (was) start_threads(f);
    LeaveCriticalSection(&f->lock);
    return S_OK;
}

/* ---------- IMediaSeeking (on pins) ---------- */
static HRESULT WINAPI SK_QI(IMediaSeeking *t, REFIID r, void **o);
static ULONG WINAPI SK_AddRef(IMediaSeeking *t) { return IBaseFilter_AddRef(&P_SK(t)->f->bf); }
static ULONG WINAPI SK_Release(IMediaSeeking *t) { return IBaseFilter_Release(&P_SK(t)->f->bf); }
static HRESULT WINAPI SK_GetCapabilities(IMediaSeeking *t, DWORD *c) { *c = AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanSeekForwards | AM_SEEKING_CanSeekBackwards | AM_SEEKING_CanGetCurrentPos | AM_SEEKING_CanGetStopPos | AM_SEEKING_CanGetDuration; return S_OK; }
static HRESULT WINAPI SK_CheckCapabilities(IMediaSeeking *t, DWORD *c)
{
    DWORD have; SK_GetCapabilities(t, &have);
    DWORD want = *c, m = want & have; *c = m;
    return m ? (m == want ? S_OK : S_FALSE) : E_FAIL;
}
static HRESULT WINAPI SK_IsFormatSupported(IMediaSeeking *t, const GUID *g) { return IsEqualGUID(g, &G_TimeFmt) ? S_OK : S_FALSE; }
static HRESULT WINAPI SK_QueryPreferredFormat(IMediaSeeking *t, GUID *g) { *g = G_TimeFmt; return S_OK; }
static HRESULT WINAPI SK_GetTimeFormat(IMediaSeeking *t, GUID *g) { *g = G_TimeFmt; return S_OK; }
static HRESULT WINAPI SK_IsUsingTimeFormat(IMediaSeeking *t, const GUID *g) { return IsEqualGUID(g, &G_TimeFmt) ? S_OK : S_FALSE; }
static HRESULT WINAPI SK_SetTimeFormat(IMediaSeeking *t, const GUID *g) { return IsEqualGUID(g, &G_TimeFmt) ? S_OK : E_INVALIDARG; }
static HRESULT WINAPI SK_GetDuration(IMediaSeeking *t, LONGLONG *d) { *d = P_SK(t)->f->duration; return S_OK; }
static HRESULT WINAPI SK_GetStopPosition(IMediaSeeking *t, LONGLONG *d) { Filter *f = P_SK(t)->f; *d = f->stop ? f->stop : f->duration; return S_OK; }
static HRESULT WINAPI SK_GetCurrentPosition(IMediaSeeking *t, LONGLONG *d) { *d = P_SK(t)->f->curpos; return S_OK; }
static HRESULT WINAPI SK_ConvertTimeFormat(IMediaSeeking *t, LONGLONG *tg, const GUID *tf, LONGLONG src, const GUID *sf)
{
    if ((tf && !IsEqualGUID(tf, &G_TimeFmt) && !IsEqualGUID(tf, &GUID_NULL)) || (sf && !IsEqualGUID(sf, &G_TimeFmt) && !IsEqualGUID(sf, &GUID_NULL))) return E_INVALIDARG;
    *tg = src; return S_OK;
}
static HRESULT WINAPI SK_SetPositions(IMediaSeeking *t, LONGLONG *cur, DWORD cf, LONGLONG *stop, DWORD sf)
{
    Filter *f = P_SK(t)->f;
    LONGLONG nc = f->segstart, ns = f->stop ? f->stop : f->duration;
    int dostart = 0, dostop = 0;
    switch (cf & 3) {
    case 1: nc = *cur; dostart = 1; break;
    case 2: case 3: nc = f->curpos + *cur; dostart = 1; break;
    }
    switch (sf & 3) {
    case 1: ns = *stop; dostop = 1; break;
    case 2: ns = f->duration + *stop; dostop = 1; break;
    case 3: ns = (f->stop ? f->stop : f->duration) + *stop; dostop = 1; break;
    }
    logmsg("SetPositions cf=%lx cur=%lld sf=%lx stop=%lld", cf, cur ? *cur : -1, sf, stop ? *stop : -1);
    if ((dostart || dostop) && !(dostart && !dostop && nc == f->segstart && GetTickCount() - f->lastseek < 100)) f->lastseek = GetTickCount(), do_seek(f, dostart ? nc : f->segstart, ns, dostop);
    if (cf & AM_SEEKING_ReturnTime && cur) *cur = f->segstart;
    return S_OK;
}
static HRESULT WINAPI SK_GetPositions(IMediaSeeking *t, LONGLONG *c, LONGLONG *s)
{
    Filter *f = P_SK(t)->f;
    if (c) *c = f->curpos;
    if (s) *s = f->stop ? f->stop : f->duration;
    return S_OK;
}
static HRESULT WINAPI SK_GetAvailable(IMediaSeeking *t, LONGLONG *e, LONGLONG *l) { if (e) *e = 0; if (l) *l = P_SK(t)->f->duration; return S_OK; }
static HRESULT WINAPI SK_SetRate(IMediaSeeking *t, double r) { return r == 1.0 ? S_OK : E_INVALIDARG; }
static HRESULT WINAPI SK_GetRate(IMediaSeeking *t, double *r) { *r = 1.0; return S_OK; }
static HRESULT WINAPI SK_GetPreroll(IMediaSeeking *t, LONGLONG *p) { *p = 0; return S_OK; }
static const IMediaSeekingVtbl sk_vtbl = {SK_QI, SK_AddRef, SK_Release, SK_GetCapabilities, SK_CheckCapabilities, SK_IsFormatSupported,
    SK_QueryPreferredFormat, SK_GetTimeFormat, SK_IsUsingTimeFormat, SK_SetTimeFormat, SK_GetDuration, SK_GetStopPosition,
    SK_GetCurrentPosition, SK_ConvertTimeFormat, SK_SetPositions, SK_GetPositions, SK_GetAvailable, SK_SetRate, SK_GetRate, SK_GetPreroll};

/* ---------- IPin ---------- */
static HRESULT WINAPI PN_QI(IPin *t, REFIID r, void **o)
{
    Pin *p = P_PIN(t);
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IPin)) *o = &p->pin;
    else if (IsEqualGUID(r, &IID_IMediaSeeking)) *o = &p->seek;
    else { *o = NULL; return E_NOINTERFACE; }
    IBaseFilter_AddRef(&p->f->bf);
    return S_OK;
}
static HRESULT WINAPI SK_QI(IMediaSeeking *t, REFIID r, void **o) { return PN_QI(&P_SK(t)->pin, r, o); }
static ULONG WINAPI PN_AddRef(IPin *t) { return IBaseFilter_AddRef(&P_PIN(t)->f->bf); }
static ULONG WINAPI PN_Release(IPin *t) { return IBaseFilter_Release(&P_PIN(t)->f->bf); }

static HRESULT negotiate_alloc(Pin *p)
{
    HRESULT hr;
    IMemInputPin *mip = NULL; IMemAllocator *al = NULL;
    ALLOCATOR_PROPERTIES req, want, act;
    hr = IPin_QueryInterface(p->peer, &IID_IMemInputPin, (void **)&mip);
    if (FAILED(hr)) return hr;
    memset(&req, 0, sizeof req);
    IMemInputPin_GetAllocatorRequirements(mip, &req);
    hr = IMemInputPin_GetAllocator(mip, &al);
    if (FAILED(hr) || !al) {
        al = NULL;
        hr = CoCreateInstance(&G_MemAllocClsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMemAllocator, (void **)&al);
        if (FAILED(hr)) { IMemInputPin_Release(mip); return hr; }
    }
    int w = p->w, h = p->h;
    DWORD vsize = ((w * 24 + 31) / 32) * 4 * h;
    want.cBuffers = req.cBuffers > 0 ? req.cBuffers : (p->kind ? 4 : 8);
    if (p->kind == 1 && want.cBuffers < 3) want.cBuffers = 3;
    want.cbBuffer = p->kind ? vsize : ACHUNK * p->ach * 2;
    if ((DWORD)req.cbBuffer > (DWORD)want.cbBuffer) want.cbBuffer = req.cbBuffer;
    want.cbAlign = req.cbAlign > 0 ? req.cbAlign : 1;
    want.cbPrefix = req.cbPrefix;
    hr = IMemAllocator_SetProperties(al, &want, &act);
    logmsg("pin %d: SetProperties(buf=%ld size=%ld) -> %08lx act buf=%ld size=%ld", p->kind, want.cBuffers, want.cbBuffer, hr, act.cBuffers, act.cbBuffer);
    if (FAILED(hr)) { IMemAllocator_Release(al); IMemInputPin_Release(mip); return hr; }
    hr = IMemInputPin_NotifyAllocator(mip, al, FALSE);
    if (FAILED(hr)) { IMemAllocator_Release(al); IMemInputPin_Release(mip); return hr; }
    p->mip = mip; p->alloc = al; p->ap = act;
    return S_OK;
}
static HRESULT WINAPI PN_Connect(IPin *t, IPin *recv, const AM_MEDIA_TYPE *pmt)
{
    Pin *p = P_PIN(t);
    AM_MEDIA_TYPE mt;
    HRESULT hr;
    logmsg("pin %d: Connect(%p) pmt=%s/%s", p->kind, recv, pmt ? guidstr(&pmt->majortype) : "-", pmt ? guidstr(&pmt->subtype) : "-");
    if (p->peer) return VFW_E_ALREADY_CONNECTED_;
    if (p->f->state != State_Stopped) return VFW_E_NOT_STOPPED_;
    if (!mt_ok(p, pmt)) return VFW_E_TYPE_NOT_ACCEPTED_;
    fill_mt(p, &mt);
    hr = IPin_ReceiveConnection(recv, &p->pin, &mt);
    logmsg("pin %d: ReceiveConnection -> %08lx", p->kind, hr);
    if (FAILED(hr)) { free_mt(&mt); return hr; }
    IPin_AddRef(recv); p->peer = recv;
    hr = negotiate_alloc(p);
    if (FAILED(hr)) {
        logmsg("pin %d: allocator negotiation failed %08lx", p->kind, hr);
        IPin_Disconnect(recv); IPin_Release(recv); p->peer = NULL; free_mt(&mt);
        return hr;
    }
    free_mt(&mt);
    logmsg("pin %d: connected", p->kind);
    return S_OK;
}
static HRESULT WINAPI PN_ReceiveConnection(IPin *t, IPin *c, const AM_MEDIA_TYPE *m) { return E_UNEXPECTED; }
static HRESULT WINAPI PN_Disconnect(IPin *t)
{
    Pin *p = P_PIN(t);
    logmsg("pin %d: Disconnect", p->kind);
    if (p->f->state != State_Stopped) return VFW_E_NOT_STOPPED_;
    if (!p->peer) return S_FALSE;
    if (p->mip) { IMemInputPin_Release(p->mip); p->mip = NULL; }
    if (p->alloc) { IMemAllocator_Release(p->alloc); p->alloc = NULL; }
    IPin_Release(p->peer); p->peer = NULL;
    return S_OK;
}
static HRESULT WINAPI PN_ConnectedTo(IPin *t, IPin **o)
{
    Pin *p = P_PIN(t);
    if (!p->peer) { *o = NULL; return VFW_E_NOT_CONNECTED_; }
    IPin_AddRef(p->peer); *o = p->peer; return S_OK;
}
static HRESULT WINAPI PN_ConnectionMediaType(IPin *t, AM_MEDIA_TYPE *m)
{
    Pin *p = P_PIN(t);
    if (!p->peer) { memset(m, 0, sizeof *m); return VFW_E_NOT_CONNECTED_; }
    fill_mt(p, m); return S_OK;
}
static HRESULT WINAPI PN_QueryPinInfo(IPin *t, PIN_INFO *i)
{
    Pin *p = P_PIN(t);
    i->pFilter = &p->f->bf; IBaseFilter_AddRef(i->pFilter);
    i->dir = PINDIR_OUTPUT; lstrcpyW(i->achName, p->name);
    return S_OK;
}
static HRESULT WINAPI PN_QueryDirection(IPin *t, PIN_DIRECTION *d) { *d = PINDIR_OUTPUT; return S_OK; }
static HRESULT WINAPI PN_QueryId(IPin *t, LPWSTR *id)
{
    Pin *p = P_PIN(t);
    *id = CoTaskMemAlloc((lstrlenW(p->name) + 1) * 2); lstrcpyW(*id, p->name); return S_OK;
}
static HRESULT WINAPI PN_QueryAccept(IPin *t, const AM_MEDIA_TYPE *m) { return mt_ok(P_PIN(t), m) ? S_OK : S_FALSE; }
static HRESULT WINAPI PN_EnumMediaTypes(IPin *t, IEnumMediaTypes **o)
{
    EMT *e = calloc(1, sizeof *e);
    e->e.lpVtbl = &emt_vtbl; e->ref = 1; e->p = P_PIN(t);
    *o = &e->e; return S_OK;
}
static HRESULT WINAPI PN_QueryInternalConnections(IPin *t, IPin **p, ULONG *n) { return E_NOTIMPL; }
static HRESULT WINAPI PN_EndOfStream(IPin *t) { return E_UNEXPECTED; }
static HRESULT WINAPI PN_BeginFlush(IPin *t) { return E_UNEXPECTED; }
static HRESULT WINAPI PN_EndFlush(IPin *t) { return E_UNEXPECTED; }
static HRESULT WINAPI PN_NewSegment(IPin *t, REFERENCE_TIME a, REFERENCE_TIME b, double r) { return E_UNEXPECTED; }
static const IPinVtbl pin_vtbl = {PN_QI, PN_AddRef, PN_Release, PN_Connect, PN_ReceiveConnection, PN_Disconnect, PN_ConnectedTo,
    PN_ConnectionMediaType, PN_QueryPinInfo, PN_QueryDirection, PN_QueryId, PN_QueryAccept, PN_EnumMediaTypes,
    PN_QueryInternalConnections, PN_EndOfStream, PN_BeginFlush, PN_EndFlush, PN_NewSegment};

/* ---------- IEnumPins ---------- */
typedef struct { IEnumPins e; LONG ref; Filter *f; int idx; } EP;
static HRESULT WINAPI EP_QI(IEnumPins *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IEnumPins)) { *o = t; ((EP *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI EP_AddRef(IEnumPins *t) { return ++((EP *)t)->ref; }
static ULONG WINAPI EP_Release(IEnumPins *t)
{
    EP *e = (EP *)t;
    if (!--e->ref) { IBaseFilter_Release(&e->f->bf); free(e); return 0; }
    return e->ref;
}
static HRESULT WINAPI EP_Next(IEnumPins *t, ULONG n, IPin **pins, ULONG *got)
{
    EP *e = (EP *)t; ULONG k = 0;
    while (k < n && e->idx < e->f->npins) {
        Pin *p = e->f->order[e->idx++];
        IPin_AddRef(&p->pin); pins[k++] = &p->pin;
    }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI EP_Skip(IEnumPins *t, ULONG n) { EP *e = (EP *)t; e->idx += n; return e->idx > e->f->npins ? S_FALSE : S_OK; }
static HRESULT WINAPI EP_Reset(IEnumPins *t) { ((EP *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI EP_Clone(IEnumPins *t, IEnumPins **o)
{
    EP *e = (EP *)t, *n = calloc(1, sizeof *n);
    *n = *e; n->ref = 1; IBaseFilter_AddRef(&n->f->bf); *o = &n->e; return S_OK;
}
static const IEnumPinsVtbl ep_vtbl = {EP_QI, EP_AddRef, EP_Release, EP_Next, EP_Skip, EP_Reset, EP_Clone};

/* ---------- IBaseFilter ---------- */
static void close_file(Filter *f)
{
    int i;
    stop_threads(f, 0);
    for (i = 0; i < 2; i++) {
        Pin *p = &f->pins[i];
        q_clear(p);
        if (p->dec) avcodec_free_context(&p->dec);
        if (p->sws) { sws_freeContext(p->sws); p->sws = NULL; }
        if (p->swr) swr_free(&p->swr);
    }
    f->npins = 0;
    if (f->fc) avformat_close_input(&f->fc);
    if (f->pkt) av_packet_free(&f->pkt);
    if (f->frame) av_frame_free(&f->frame);
    free(f->file); f->file = NULL;
}
static HRESULT WINAPI BF_QI(IBaseFilter *t, REFIID r, void **o)
{
    Filter *f = F_BF(t);
    if (IsEqualGUID(r, &G_IUnknown_) || IsEqualGUID(r, &IID_IPersist) || IsEqualGUID(r, &IID_IMediaFilter) || IsEqualGUID(r, &IID_IBaseFilter)) *o = &f->bf;
    else if (IsEqualGUID(r, &IID_IFileSourceFilter)) *o = &f->fsf;
    else if (IsEqualGUID(r, &G_IMiscFlags)) *o = &f->mf;
    else if (IsEqualGUID(r, &IID_IMediaSeeking) && f->npins) *o = &f->order[f->npins - 1]->seek;
    else { logmsg("Filter QI %s -> E_NOINTERFACE", guidstr(r)); *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&f->ref);
    return S_OK;
}
static ULONG WINAPI BF_AddRef(IBaseFilter *t) { return InterlockedIncrement(&F_BF(t)->ref); }
static ULONG WINAPI BF_Release(IBaseFilter *t)
{
    Filter *f = F_BF(t);
    LONG n = InterlockedDecrement(&f->ref);
    if (!n) {
        int i;
        logmsg("Filter destroyed");
        close_file(f);
        for (i = 0; i < 2; i++) {
            if (f->pins[i].mip) IMemInputPin_Release(f->pins[i].mip);
            if (f->pins[i].alloc) IMemAllocator_Release(f->pins[i].alloc);
            if (f->pins[i].peer) IPin_Release(f->pins[i].peer);
        }
        DeleteCriticalSection(&f->lock); DeleteCriticalSection(&f->qlock);
        free(f);
    }
    return n;
}
static HRESULT WINAPI BF_GetClassID(IBaseFilter *t, CLSID *c) { *c = CLSID_WMAsfReader_; return S_OK; }
static HRESULT WINAPI BF_Stop(IBaseFilter *t)
{
    Filter *f = F_BF(t);
    logmsg("Stop");
    EnterCriticalSection(&f->lock);
    stop_threads(f, 0);
    f->state = State_Stopped;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_Pause(IBaseFilter *t)
{
    Filter *f = F_BF(t);
    logmsg("Pause (state %d)", f->state);
    EnterCriticalSection(&f->lock);
    if (f->state == State_Stopped) start_threads(f);
    f->state = State_Paused;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_Run(IBaseFilter *t, REFERENCE_TIME start)
{
    Filter *f = F_BF(t);
    logmsg("Run(%lld) (state %d)", start, f->state);
    EnterCriticalSection(&f->lock);
    if (f->state == State_Stopped) start_threads(f);
    f->state = State_Running;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_GetState(IBaseFilter *t, DWORD ms, FILTER_STATE *s) { *s = F_BF(t)->state; return S_OK; }
static HRESULT WINAPI BF_SetSyncSource(IBaseFilter *t, IReferenceClock *c)
{
    Filter *f = F_BF(t);
    if (c) IReferenceClock_AddRef(c);
    if (f->clock) IReferenceClock_Release(f->clock);
    f->clock = c; return S_OK;
}
static HRESULT WINAPI BF_GetSyncSource(IBaseFilter *t, IReferenceClock **c)
{
    Filter *f = F_BF(t);
    if (f->clock) IReferenceClock_AddRef(f->clock);
    *c = f->clock; return S_OK;
}
static HRESULT WINAPI BF_EnumPins(IBaseFilter *t, IEnumPins **o)
{
    EP *e = calloc(1, sizeof *e);
    e->e.lpVtbl = &ep_vtbl; e->ref = 1; e->f = F_BF(t); IBaseFilter_AddRef(t);
    *o = &e->e; return S_OK;
}
static HRESULT WINAPI BF_FindPin(IBaseFilter *t, LPCWSTR id, IPin **o)
{
    Filter *f = F_BF(t);
    for (int i = 0; i < f->npins; i++) if (!lstrcmpiW(f->order[i]->name, id)) { IPin_AddRef(&f->order[i]->pin); *o = &f->order[i]->pin; return S_OK; }
    *o = NULL; return VFW_E_NOT_FOUND_;
}
static HRESULT WINAPI BF_QueryFilterInfo(IBaseFilter *t, FILTER_INFO *i)
{
    Filter *f = F_BF(t);
    lstrcpyW(i->achName, f->gname);
    i->pGraph = f->graph; if (f->graph) IFilterGraph_AddRef(f->graph);
    return S_OK;
}
static HRESULT WINAPI BF_JoinFilterGraph(IBaseFilter *t, IFilterGraph *g, LPCWSTR name)
{
    Filter *f = F_BF(t);
    f->graph = g;
    if (name) lstrcpynW(f->gname, name, 128); else f->gname[0] = 0;
    logmsg("JoinFilterGraph(%p)", g);
    return S_OK;
}
static HRESULT WINAPI BF_QueryVendorInfo(IBaseFilter *t, LPWSTR *v) { return E_NOTIMPL; }
static const IBaseFilterVtbl bf_vtbl = {BF_QI, BF_AddRef, BF_Release, BF_GetClassID, BF_Stop, BF_Pause, BF_Run, BF_GetState,
    BF_SetSyncSource, BF_GetSyncSource, BF_EnumPins, BF_FindPin, BF_QueryFilterInfo, BF_JoinFilterGraph, BF_QueryVendorInfo};

/* IFileSourceFilter */
static HRESULT WINAPI FS_QI(IFileSourceFilter *t, REFIID r, void **o) { return BF_QI(&F_FS(t)->bf, r, o); }
static ULONG WINAPI FS_AddRef(IFileSourceFilter *t) { return BF_AddRef(&F_FS(t)->bf); }
static ULONG WINAPI FS_Release(IFileSourceFilter *t) { return BF_Release(&F_FS(t)->bf); }
static HRESULT WINAPI FS_Load(IFileSourceFilter *t, LPCOLESTR fn, const AM_MEDIA_TYPE *mt)
{
    Filter *f = F_FS(t);
    char u[1024];
    int i, k, np = 0;
    WideCharToMultiByte(CP_UTF8, 0, fn, -1, u, sizeof u, NULL, NULL);
    logmsg("Load(%s)", u);
    EnterCriticalSection(&f->lock);
    close_file(f);
    av_log_set_level(AV_LOG_QUIET);
    if (avformat_open_input(&f->fc, u, NULL, NULL) < 0) { f->fc = NULL; LeaveCriticalSection(&f->lock); logmsg("open failed"); return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); }
    avformat_find_stream_info(f->fc, NULL);
    f->pkt = av_packet_alloc(); f->frame = av_frame_alloc();
    f->duration = f->fc->duration > 0 ? (LONGLONG)f->fc->duration * 10 : 0;
    f->segstart = 0; f->stop = 0; f->curpos = 0;
    for (k = 0; k < 2; k++) {
        for (i = 0; i < (int)f->fc->nb_streams; i++) {
            AVStream *st = f->fc->streams[i];
            int want = k == 0 ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
            if (st->codecpar->codec_type != want || np >= 2) continue;
            const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
            if (!c) continue;
            Pin *p = &f->pins[np];
            memset(p, 0, sizeof *p);
            p->pin.lpVtbl = &pin_vtbl; p->seek.lpVtbl = &sk_vtbl; p->f = f; p->kind = k; p->sidx = i; p->st = st;
            p->dec = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(p->dec, st->codecpar);
            p->dec->thread_count = 1;
            if (avcodec_open2(p->dec, c, NULL) < 0) { avcodec_free_context(&p->dec); continue; }
            if (k == 0) { p->arate = st->codecpar->sample_rate; p->ach = st->codecpar->ch_layout.nb_channels ? st->codecpar->ch_layout.nb_channels : 2; }
            else { p->w = st->codecpar->width; p->h = st->codecpar->height; }
            wsprintfW(p->name, k ? L"Raw Video %d" : L"Raw Audio %d", st->id ? st->id : i + 1);
            f->order[np] = p; np++;
            logmsg("pin %d: %s stream %d codec %s", np - 1, k ? "video" : "audio", i, c->name);
        }
    }
    f->npins = np;
    f->file = _wcsdup(fn);
    LeaveCriticalSection(&f->lock);
    logmsg("loaded: duration %lld npins %d", f->duration, np);
    return np ? S_OK : E_FAIL;
}
static HRESULT WINAPI FS_GetCurFile(IFileSourceFilter *t, LPOLESTR *fn, AM_MEDIA_TYPE *mt)
{
    Filter *f = F_FS(t);
    if (f->file) { *fn = CoTaskMemAlloc((lstrlenW(f->file) + 1) * 2); lstrcpyW(*fn, f->file); } else *fn = NULL;
    if (mt) memset(mt, 0, sizeof *mt);
    return S_OK;
}
static const IFileSourceFilterVtbl fs_vtbl = {FS_QI, FS_AddRef, FS_Release, FS_Load, FS_GetCurFile};

static HRESULT WINAPI MF_QI(IAMFilterMiscFlags *t, REFIID r, void **o) { return BF_QI(&F_MF(t)->bf, r, o); }
static ULONG WINAPI MF_AddRef(IAMFilterMiscFlags *t) { return BF_AddRef(&F_MF(t)->bf); }
static ULONG WINAPI MF_Release(IAMFilterMiscFlags *t) { return BF_Release(&F_MF(t)->bf); }
static ULONG WINAPI MF_Get(IAMFilterMiscFlags *t) { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }
static const IAMFilterMiscFlagsVtbl mf_vtbl = {MF_QI, MF_AddRef, MF_Release, MF_Get};

/* ---------- class factory / exports ---------- */
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
    logmsg("CreateInstance(%s)", guidstr(r));
    if (outer) return CLASS_E_NOAGGREGATION;
    Filter *f = calloc(1, sizeof *f);
    f->bf.lpVtbl = &bf_vtbl; f->fsf.lpVtbl = &fs_vtbl; f->mf.lpVtbl = &mf_vtbl; f->ref = 1;
    InitializeCriticalSection(&f->lock); InitializeCriticalSection(&f->qlock); InitializeConditionVariable(&f->qcv);
    f->state = State_Stopped;
    HRESULT hr = BF_QI(&f->bf, r, o);
    BF_Release(&f->bf);
    return hr;
}
static HRESULT WINAPI CF_Lock(IClassFactory *t, BOOL l) { return S_OK; }
static const IClassFactoryVtbl cf_vtbl = {CF_QI, CF_AddRef, CF_Release, CF_Create, CF_Lock};
static CF the_cf = {{&cf_vtbl}};

int writer_clsid(REFCLSID c);
HRESULT writer_get_factory(REFIID r, void **o);
int devenum_clsid(REFCLSID c);
HRESULT devenum_get_factory(REFIID r, void **o);
int filewriter_clsid(REFCLSID c);
HRESULT filewriter_get_factory(REFIID r, void **o);
int wavparse_clsid(REFCLSID c);
HRESULT wavparse_get_factory(REFIID r, void **o);
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, void **o)
{
    logmsg("DllGetClassObject(%s)", guidstr(c));
    if (writer_clsid(c)) return writer_get_factory(r, o);
    if (filewriter_clsid(c)) return filewriter_get_factory(r, o);
    if (devenum_clsid(c)) return devenum_get_factory(r, o);
    if (wavparse_clsid(c)) return wavparse_get_factory(r, o);
    if (!IsEqualGUID(c, &CLSID_WMAsfReader_)) return CLASS_E_CLASSNOTAVAILABLE;
    return CF_QI(&the_cf.cf, r, o);
}
HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, void *res) { return TRUE; }
