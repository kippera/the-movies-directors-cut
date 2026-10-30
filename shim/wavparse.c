/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DirectShow "Wav Parser" (CLSID_WAVEParser) replacement.
 * Wine's built-in one runs a GStreamer pipeline (wg_parser). During an export ("Saving Sound Data") that pipeline
 * intermittently deadlocks in GStreamer's typefind / pad removal (main thread stuck in wg_parser_connect) or crashes in
 * a gstglcontext thread. This filter reads the WAV through the upstream IAsyncReader itself and pushes plain PCM
 * downstream, so GStreamer is never started.
 * Graph as the game builds it:  File Reader (IAsyncReader) -> [this] -> Audio Enc (ACM wrapper) -> ASF writer. */
#define COBJMACROS
#define STRSAFE_NO_DEPRECATE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <dshow.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

void logmsg(const char *fmt, ...);
const char *guidstr(const GUID *g);

static const CLSID CLSID_WavParse_ = {0xd51bd5a1, 0x7548, 0x11cf, {0xa5, 0x20, 0x00, 0x80, 0xc7, 0x7e, 0xf5, 0x8a}};
static const GUID G_MemAllocClsid_ = {0x1e651cc0, 0xb199, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
static const GUID G_IUnk_ = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID G_TimeFmt_ = {0x7b785570, 0x8c82, 0x11cf, {0xbc, 0x0c, 0x00, 0xaa, 0x00, 0xac, 0x74, 0xf6}}; /* TIME_FORMAT_MEDIA_TIME */
static const GUID G_MajAudio_ = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID G_FmtWfx_ = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
#define E_NOT_CONNECTED ((HRESULT)0x80040209)
#define E_NOT_ACCEPTED ((HRESULT)0x8004022A)
#define E_NOT_FOUND ((HRESULT)0x80040216)
#define E_ALREADY_CONN ((HRESULT)0x80040204)
#define E_NOT_STOPPED ((HRESULT)0x80040225)

typedef struct WP WP;
struct WP {
    IBaseFilter bf;
    IPin inpin, outpin;
    IMediaSeeking ms;
    LONG ref;
    CRITICAL_SECTION lock;
    FILTER_STATE state;
    IFilterGraph *graph; WCHAR gname[128];
    IPin *inpeer; IAsyncReader *rd;
    IPin *outpeer; IMemInputPin *mip; IMemAllocator *alloc; LONG chunk;
    AM_MEDIA_TYPE mt; WAVEFORMATEX *wfx; DWORD wfxsize;
    LONGLONG data_off, data_len;
    LONGLONG startb, stopb, curb;    /* byte positions inside the data chunk */
    HANDLE th, ev_run, ev_stop;
};
#define F_BF(p) ((WP *)(p))
#define F_IN(p) ((WP *)((BYTE *)(p) - offsetof(WP, inpin)))
#define F_OUT(p) ((WP *)((BYTE *)(p) - offsetof(WP, outpin)))
#define F_MS(p) ((WP *)((BYTE *)(p) - offsetof(WP, ms)))

static LONGLONG b2t(WP *f, LONGLONG b) { return f->wfx && f->wfx->nAvgBytesPerSec ? b * 10000000LL / f->wfx->nAvgBytesPerSec : 0; }
static LONGLONG t2b(WP *f, LONGLONG t)
{
    LONGLONG b = f->wfx ? t * f->wfx->nAvgBytesPerSec / 10000000LL : 0;
    if (f->wfx && f->wfx->nBlockAlign) b -= b % f->wfx->nBlockAlign;
    if (b < 0) b = 0;
    if (b > f->data_len) b = f->data_len;
    return b;
}
static void free_mt(AM_MEDIA_TYPE *m) { if (m->pbFormat) CoTaskMemFree(m->pbFormat); memset(m, 0, sizeof *m); }
static HRESULT copy_mt(const AM_MEDIA_TYPE *s, AM_MEDIA_TYPE *d)
{
    *d = *s; d->pUnk = NULL; d->pbFormat = NULL;
    if (s->cbFormat && s->pbFormat) { d->pbFormat = CoTaskMemAlloc(s->cbFormat); if (!d->pbFormat) return E_OUTOFMEMORY; memcpy(d->pbFormat, s->pbFormat, s->cbFormat); }
    return S_OK;
}

/* ---------- parse the RIFF/WAVE header through IAsyncReader ---------- */
static int wp_parse(WP *f)
{
    LONGLONG total = 0, avail = 0, pos = 12;
    BYTE hdr[12], ch[8], fmt[64]; int have_fmt = 0, have_data = 0; DWORD fmtlen = 0;
    if (FAILED(IAsyncReader_Length(f->rd, &total, &avail)) || total < 44) return 0;
    if (FAILED(IAsyncReader_SyncRead(f->rd, 0, 12, hdr))) return 0;
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return 0;
    while (pos + 8 <= total && !(have_fmt && have_data)) {
        DWORD sz;
        if (FAILED(IAsyncReader_SyncRead(f->rd, pos, 8, ch))) return 0;
        sz = *(DWORD *)(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            DWORD n = sz < sizeof fmt ? sz : (DWORD)sizeof fmt;
            if (n < 14 || FAILED(IAsyncReader_SyncRead(f->rd, pos + 8, n, fmt))) return 0;
            fmtlen = n; have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            f->data_off = pos + 8; f->data_len = sz;
            if (f->data_off + f->data_len > total || sz == 0 || sz == 0xFFFFFFFFu) f->data_len = total - f->data_off;
            have_data = 1;
            if (!have_fmt) { pos += 8 + (LONGLONG)sz + (sz & 1); continue; }
            break;
        }
        pos += 8 + (LONGLONG)sz + (sz & 1);
    }
    if (!have_fmt || !have_data) return 0;
    {
        DWORD sz = fmtlen < 16 ? 18 : fmtlen < 18 ? 18 : fmtlen;
        f->wfx = CoTaskMemAlloc(sz); memset(f->wfx, 0, sz); memcpy(f->wfx, fmt, fmtlen);
        if (fmtlen <= 16) f->wfx->cbSize = 0;
        f->wfxsize = sz;
        if (!f->wfx->nBlockAlign) f->wfx->nBlockAlign = 1;
        if (f->wfx->wFormatTag == WAVE_FORMAT_PCM) f->wfx->nAvgBytesPerSec = f->wfx->nSamplesPerSec * f->wfx->nBlockAlign;
        f->data_len -= f->data_len % f->wfx->nBlockAlign;
    }
    memset(&f->mt, 0, sizeof f->mt);
    f->mt.majortype = G_MajAudio_;
    f->mt.subtype = G_IUnk_; f->mt.subtype.Data1 = f->wfx->wFormatTag; f->mt.subtype.Data2 = 0; f->mt.subtype.Data3 = 0x10;
    { static const BYTE tail[8] = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}; memcpy(f->mt.subtype.Data4, tail, 8); }
    f->mt.bFixedSizeSamples = TRUE; f->mt.lSampleSize = f->wfx->nBlockAlign; f->mt.formattype = G_FmtWfx_;
    f->mt.cbFormat = f->wfxsize; f->mt.pbFormat = CoTaskMemAlloc(f->wfxsize); memcpy(f->mt.pbFormat, f->wfx, f->wfxsize);
    f->startb = 0; f->stopb = f->data_len; f->curb = 0;
    logmsg("WavParse: tag=%u ch=%u rate=%lu bits=%u align=%u data_off=%lld data_len=%lld (%.2fs)", f->wfx->wFormatTag, f->wfx->nChannels,
           f->wfx->nSamplesPerSec, f->wfx->wBitsPerSample, f->wfx->nBlockAlign, (long long)f->data_off, (long long)f->data_len, b2t(f, f->data_len) / 1e7);
    return 1;
}

/* ---------- streaming thread ---------- */
static DWORD WINAPI wp_thread(void *arg)
{
    WP *f = arg; HANDLE w[2]; LONGLONG pos = f->startb, stop = f->stopb; int first = 1, ok = 1;
    IPin_NewSegment(f->outpeer, b2t(f, pos), b2t(f, stop), 1.0);
    w[0] = f->ev_run; w[1] = f->ev_stop;
    if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) return 0;
    logmsg("WavParse: streaming %lld..%lld", (long long)pos, (long long)stop);
    while (pos < stop && WaitForSingleObject(f->ev_stop, 0) == WAIT_TIMEOUT) {
        IMediaSample *ms = NULL; BYTE *p; LONG n; HRESULT hr; LONGLONG t0, t1;
        hr = IMemAllocator_GetBuffer(f->alloc, &ms, NULL, NULL, 0);
        if (FAILED(hr) || !ms) { ok = 0; break; }
        IMediaSample_GetPointer(ms, &p);
        n = IMediaSample_GetSize(ms);
        if (n > f->chunk) n = f->chunk;
        if (n > stop - pos) n = (LONG)(stop - pos);
        n -= n % f->wfx->nBlockAlign;
        if (n <= 0 || FAILED(IAsyncReader_SyncRead(f->rd, f->data_off + pos, n, p))) { IMediaSample_Release(ms); ok = 0; break; }
        IMediaSample_SetActualDataLength(ms, n);
        t0 = b2t(f, pos); t1 = b2t(f, pos + n);
        IMediaSample_SetTime(ms, &t0, &t1);
        IMediaSample_SetSyncPoint(ms, TRUE); IMediaSample_SetPreroll(ms, FALSE); IMediaSample_SetDiscontinuity(ms, first);
        first = 0;
        hr = IMemInputPin_Receive(f->mip, ms);
        IMediaSample_Release(ms);
        pos += n; f->curb = pos;
        if (hr != S_OK) { ok = 0; break; }
    }
    if (ok && pos >= stop) { logmsg("WavParse: end of stream"); IPin_EndOfStream(f->outpeer); }
    else logmsg("WavParse: streaming stopped at %lld (ok=%d)", (long long)pos, ok);
    return 0;
}
static void wp_stop_thread(WP *f)
{
    if (!f->th) return;
    SetEvent(f->ev_stop);
    if (f->outpeer) IPin_BeginFlush(f->outpeer);
    if (f->alloc) IMemAllocator_Decommit(f->alloc);
    if (WaitForSingleObject(f->th, 5000) == WAIT_TIMEOUT) logmsg("WavParse: thread did not stop");
    CloseHandle(f->th); f->th = NULL;
    if (f->outpeer) IPin_EndFlush(f->outpeer);
}
static void wp_start_thread(WP *f)
{
    if (f->th || !f->outpeer) return;
    ResetEvent(f->ev_run); ResetEvent(f->ev_stop);
    if (f->alloc) IMemAllocator_Commit(f->alloc);
    f->th = CreateThread(NULL, 0, wp_thread, f, 0, NULL);
}

/* ---------- input pin ---------- */
static HRESULT WINAPI IP_QI(IPin *t, REFIID r, void **o)
{
    WP *f = F_IN(t);
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IPin)) *o = &f->inpin;
    else { *o = NULL; return E_NOINTERFACE; }
    IBaseFilter_AddRef(&f->bf);
    return S_OK;
}
static ULONG WINAPI IP_AddRef(IPin *t) { return IBaseFilter_AddRef(&F_IN(t)->bf); }
static ULONG WINAPI IP_Release(IPin *t) { return IBaseFilter_Release(&F_IN(t)->bf); }
static HRESULT WINAPI IP_Connect(IPin *t, IPin *r, const AM_MEDIA_TYPE *m) { return E_UNEXPECTED; }
static HRESULT WINAPI IP_ReceiveConnection(IPin *t, IPin *conn, const AM_MEDIA_TYPE *m)
{
    WP *f = F_IN(t); IAsyncReader *rd = NULL;
    logmsg("WavParse in: ReceiveConnection major=%s sub=%s", m ? guidstr(&m->majortype) : "-", m ? guidstr(&m->subtype) : "-");
    if (f->inpeer) return E_ALREADY_CONN;
    if (f->state != State_Stopped) return E_NOT_STOPPED;
    if (FAILED(IPin_QueryInterface(conn, &IID_IAsyncReader, (void **)&rd))) return E_NOT_ACCEPTED;
    f->rd = rd;
    if (!wp_parse(f)) { logmsg("WavParse: not a usable WAV"); IAsyncReader_Release(rd); f->rd = NULL; return E_NOT_ACCEPTED; }
    IPin_AddRef(conn); f->inpeer = conn;
    return S_OK;
}
static HRESULT WINAPI IP_Disconnect(IPin *t)
{
    WP *f = F_IN(t);
    if (f->state != State_Stopped) return E_NOT_STOPPED;
    if (!f->inpeer) return S_FALSE;
    if (f->outpeer) return E_NOT_STOPPED;
    IPin_Release(f->inpeer); f->inpeer = NULL;
    if (f->rd) { IAsyncReader_Release(f->rd); f->rd = NULL; }
    if (f->wfx) { CoTaskMemFree(f->wfx); f->wfx = NULL; }
    free_mt(&f->mt);
    return S_OK;
}
static HRESULT WINAPI IP_ConnectedTo(IPin *t, IPin **o)
{
    WP *f = F_IN(t);
    if (!f->inpeer) { *o = NULL; return E_NOT_CONNECTED; }
    IPin_AddRef(f->inpeer); *o = f->inpeer; return S_OK;
}
static HRESULT WINAPI IP_ConnectionMediaType(IPin *t, AM_MEDIA_TYPE *m) { memset(m, 0, sizeof *m); return E_NOT_CONNECTED; }
static HRESULT WINAPI IP_QueryPinInfo(IPin *t, PIN_INFO *i)
{
    WP *f = F_IN(t);
    i->pFilter = &f->bf; IBaseFilter_AddRef(i->pFilter);
    i->dir = PINDIR_INPUT; lstrcpyW(i->achName, L"input"); return S_OK;
}
static HRESULT WINAPI IP_QueryDirection(IPin *t, PIN_DIRECTION *d) { *d = PINDIR_INPUT; return S_OK; }
static HRESULT WINAPI IP_QueryId(IPin *t, LPWSTR *id) { *id = CoTaskMemAlloc(12); lstrcpyW(*id, L"input"); return S_OK; }
static HRESULT WINAPI IP_QueryAccept(IPin *t, const AM_MEDIA_TYPE *m) { return S_OK; }

typedef struct { IEnumMediaTypes e; LONG ref; AM_MEDIA_TYPE mt; int have, idx; } EMT;
static HRESULT WINAPI EM_QI(IEnumMediaTypes *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IEnumMediaTypes)) { *o = t; ((EMT *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI EM_AddRef(IEnumMediaTypes *t) { return ++((EMT *)t)->ref; }
static ULONG WINAPI EM_Release(IEnumMediaTypes *t) { EMT *e = (EMT *)t; if (!--e->ref) { free_mt(&e->mt); free(e); return 0; } return e->ref; }
static HRESULT WINAPI EM_Next(IEnumMediaTypes *t, ULONG n, AM_MEDIA_TYPE **m, ULONG *g)
{
    EMT *e = (EMT *)t; ULONG k = 0;
    while (k < n && e->have && e->idx < 1) {
        AM_MEDIA_TYPE *c = CoTaskMemAlloc(sizeof *c);
        if (!c || FAILED(copy_mt(&e->mt, c))) break;
        m[k++] = c; e->idx++;
    }
    if (g) *g = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI EM_Skip(IEnumMediaTypes *t, ULONG n) { EMT *e = (EMT *)t; e->idx += n; return e->idx > e->have ? S_FALSE : S_OK; }
static HRESULT WINAPI EM_Reset(IEnumMediaTypes *t) { ((EMT *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI EM_Clone(IEnumMediaTypes *t, IEnumMediaTypes **o)
{
    EMT *s = (EMT *)t, *e = calloc(1, sizeof *e);
    e->e.lpVtbl = t->lpVtbl; e->ref = 1; e->have = s->have; e->idx = s->idx;
    if (s->have) copy_mt(&s->mt, &e->mt);
    *o = &e->e; return S_OK;
}
static const IEnumMediaTypesVtbl emt_vtbl = {EM_QI, EM_AddRef, EM_Release, EM_Next, EM_Skip, EM_Reset, EM_Clone};
static HRESULT WINAPI IP_EnumMediaTypes(IPin *t, IEnumMediaTypes **o) { EMT *e = calloc(1, sizeof *e); e->e.lpVtbl = &emt_vtbl; e->ref = 1; *o = &e->e; return S_OK; }
static HRESULT WINAPI IP_QueryInternalConnections(IPin *t, IPin **p, ULONG *n)
{
    WP *f = F_IN(t);
    if (!n || *n < 1) { if (n) *n = 1; return S_FALSE; }
    IPin_AddRef(&f->outpin); p[0] = &f->outpin; *n = 1; return S_OK;
}
static HRESULT WINAPI IP_EndOfStream(IPin *t) { return S_OK; }
static HRESULT WINAPI IP_BeginFlush(IPin *t) { return S_OK; }
static HRESULT WINAPI IP_EndFlush(IPin *t) { return S_OK; }
static HRESULT WINAPI IP_NewSegment(IPin *t, REFERENCE_TIME a, REFERENCE_TIME b, double r) { return S_OK; }
static const IPinVtbl ip_vtbl = {IP_QI, IP_AddRef, IP_Release, IP_Connect, IP_ReceiveConnection, IP_Disconnect, IP_ConnectedTo,
    IP_ConnectionMediaType, IP_QueryPinInfo, IP_QueryDirection, IP_QueryId, IP_QueryAccept, IP_EnumMediaTypes,
    IP_QueryInternalConnections, IP_EndOfStream, IP_BeginFlush, IP_EndFlush, IP_NewSegment};

/* ---------- IMediaSeeking (on the filter and on the output pin) ---------- */
static HRESULT WINAPI BF_QI(IBaseFilter *t, REFIID r, void **o);
static HRESULT WINAPI MS_QI(IMediaSeeking *t, REFIID r, void **o) { return BF_QI(&F_MS(t)->bf, r, o); }
static ULONG WINAPI MS_AddRef(IMediaSeeking *t) { return IBaseFilter_AddRef(&F_MS(t)->bf); }
static ULONG WINAPI MS_Release(IMediaSeeking *t) { return IBaseFilter_Release(&F_MS(t)->bf); }
#define SEEKCAPS (AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanSeekForwards | AM_SEEKING_CanSeekBackwards | AM_SEEKING_CanGetCurrentPos | AM_SEEKING_CanGetStopPos | AM_SEEKING_CanGetDuration)
static HRESULT WINAPI MS_GetCapabilities(IMediaSeeking *t, DWORD *c) { *c = SEEKCAPS; return S_OK; }
static HRESULT WINAPI MS_CheckCapabilities(IMediaSeeking *t, DWORD *c) { DWORD got = *c & SEEKCAPS; HRESULT hr = got == *c ? S_OK : got ? S_FALSE : E_FAIL; *c = got; return hr; }
static HRESULT WINAPI MS_IsFormatSupported(IMediaSeeking *t, const GUID *g) { return IsEqualGUID(g, &G_TimeFmt_) ? S_OK : S_FALSE; }
static HRESULT WINAPI MS_QueryPreferredFormat(IMediaSeeking *t, GUID *g) { *g = G_TimeFmt_; return S_OK; }
static HRESULT WINAPI MS_GetTimeFormat(IMediaSeeking *t, GUID *g) { *g = G_TimeFmt_; return S_OK; }
static HRESULT WINAPI MS_IsUsingTimeFormat(IMediaSeeking *t, const GUID *g) { return IsEqualGUID(g, &G_TimeFmt_) ? S_OK : S_FALSE; }
static HRESULT WINAPI MS_SetTimeFormat(IMediaSeeking *t, const GUID *g) { return IsEqualGUID(g, &G_TimeFmt_) ? S_OK : E_INVALIDARG; }
static HRESULT WINAPI MS_GetDuration(IMediaSeeking *t, LONGLONG *d) { WP *f = F_MS(t); if (!f->wfx) return E_NOT_CONNECTED; *d = b2t(f, f->data_len); return S_OK; }
static HRESULT WINAPI MS_GetStopPosition(IMediaSeeking *t, LONGLONG *d) { WP *f = F_MS(t); if (!f->wfx) return E_NOT_CONNECTED; *d = b2t(f, f->stopb); return S_OK; }
static HRESULT WINAPI MS_GetCurrentPosition(IMediaSeeking *t, LONGLONG *d) { WP *f = F_MS(t); if (!f->wfx) return E_NOT_CONNECTED; *d = b2t(f, f->state == State_Stopped ? f->startb : f->curb); return S_OK; }
static HRESULT WINAPI MS_ConvertTimeFormat(IMediaSeeking *t, LONGLONG *tg, const GUID *tf, LONGLONG src, const GUID *sf)
{
    if ((tf && !IsEqualGUID(tf, &G_TimeFmt_)) || (sf && !IsEqualGUID(sf, &G_TimeFmt_))) return E_INVALIDARG;
    *tg = src; return S_OK;
}
static HRESULT WINAPI MS_SetPositions(IMediaSeeking *t, LONGLONG *cur, DWORD cf, LONGLONG *stop, DWORD sf)
{
    WP *f = F_MS(t); FILTER_STATE st; LONGLONG ns, ne;
    if (!f->wfx) return E_NOT_CONNECTED;
    EnterCriticalSection(&f->lock);
    ns = f->startb; ne = f->stopb;
    if ((cf & 3) == AM_SEEKING_AbsolutePositioning && cur) ns = t2b(f, *cur);
    else if ((cf & 3) == AM_SEEKING_RelativePositioning && cur) ns = t2b(f, b2t(f, f->startb) + *cur);
    if ((sf & 3) == AM_SEEKING_AbsolutePositioning && stop) ne = t2b(f, *stop);
    else if ((sf & 3) == AM_SEEKING_RelativePositioning && stop) ne = t2b(f, b2t(f, f->stopb) + *stop);
    if (ne < ns) ne = ns;
    f->startb = ns; f->stopb = ne; f->curb = ns;
    st = f->state;
    LeaveCriticalSection(&f->lock);
    logmsg("WavParse: SetPositions %lld..%lld (state %d)", (long long)ns, (long long)ne, st);
    if (st != State_Stopped && f->th) {            /* restart the pump from the new position */
        wp_stop_thread(f);
        wp_start_thread(f);
        if (st == State_Running) SetEvent(f->ev_run);
    }
    return S_OK;
}
static HRESULT WINAPI MS_GetPositions(IMediaSeeking *t, LONGLONG *c, LONGLONG *s)
{
    WP *f = F_MS(t); if (!f->wfx) return E_NOT_CONNECTED;
    if (c) *c = b2t(f, f->startb); if (s) *s = b2t(f, f->stopb); return S_OK;
}
static HRESULT WINAPI MS_GetAvailable(IMediaSeeking *t, LONGLONG *e, LONGLONG *l)
{
    WP *f = F_MS(t); if (!f->wfx) return E_NOT_CONNECTED;
    if (e) *e = 0; if (l) *l = b2t(f, f->data_len); return S_OK;
}
static HRESULT WINAPI MS_SetRate(IMediaSeeking *t, double r) { return r == 1.0 ? S_OK : E_INVALIDARG; }
static HRESULT WINAPI MS_GetRate(IMediaSeeking *t, double *r) { *r = 1.0; return S_OK; }
static HRESULT WINAPI MS_GetPreroll(IMediaSeeking *t, LONGLONG *p) { *p = 0; return S_OK; }
static const IMediaSeekingVtbl ms_vtbl = {MS_QI, MS_AddRef, MS_Release, MS_GetCapabilities, MS_CheckCapabilities, MS_IsFormatSupported,
    MS_QueryPreferredFormat, MS_GetTimeFormat, MS_IsUsingTimeFormat, MS_SetTimeFormat, MS_GetDuration, MS_GetStopPosition,
    MS_GetCurrentPosition, MS_ConvertTimeFormat, MS_SetPositions, MS_GetPositions, MS_GetAvailable, MS_SetRate, MS_GetRate, MS_GetPreroll};

/* ---------- output pin ---------- */
static HRESULT WINAPI OP_QI(IPin *t, REFIID r, void **o)
{
    WP *f = F_OUT(t);
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IPin)) *o = &f->outpin;
    else if (IsEqualGUID(r, &IID_IMediaSeeking)) *o = &f->ms;
    else { *o = NULL; return E_NOINTERFACE; }
    IBaseFilter_AddRef(&f->bf);
    return S_OK;
}
static ULONG WINAPI OP_AddRef(IPin *t) { return IBaseFilter_AddRef(&F_OUT(t)->bf); }
static ULONG WINAPI OP_Release(IPin *t) { return IBaseFilter_Release(&F_OUT(t)->bf); }
static HRESULT WINAPI OP_Connect(IPin *t, IPin *recv, const AM_MEDIA_TYPE *pmt)
{
    WP *f = F_OUT(t); HRESULT hr; IMemInputPin *mip = NULL; IMemAllocator *al = NULL; ALLOCATOR_PROPERTIES rq, ac;
    LONG sz;
    if (!f->wfx) return E_NOT_CONNECTED;
    if (f->outpeer) return E_ALREADY_CONN;
    if (f->state != State_Stopped) return E_NOT_STOPPED;
    if (pmt && !IsEqualGUID(&pmt->majortype, &GUID_NULL) && !IsEqualGUID(&pmt->majortype, &G_MajAudio_)) return E_NOT_ACCEPTED;
    hr = IPin_ReceiveConnection(recv, &f->outpin, &f->mt);
    if (FAILED(hr)) { logmsg("WavParse out: ReceiveConnection refused %lx", hr); return hr; }
    hr = IPin_QueryInterface(recv, &IID_IMemInputPin, (void **)&mip);
    if (FAILED(hr)) { IPin_Disconnect(recv); return hr; }
    if (FAILED(IMemInputPin_GetAllocator(mip, &al)) || !al) {
        al = NULL;
        if (FAILED(CoCreateInstance(&G_MemAllocClsid_, NULL, CLSCTX_INPROC_SERVER, &IID_IMemAllocator, (void **)&al))) { IMemInputPin_Release(mip); IPin_Disconnect(recv); return E_FAIL; }
    }
    memset(&rq, 0, sizeof rq);
    IMemInputPin_GetAllocatorRequirements(mip, &rq);
    sz = 16384; if (rq.cbBuffer > sz) sz = rq.cbBuffer;
    sz -= sz % f->wfx->nBlockAlign;
    rq.cbBuffer = sz; if (rq.cBuffers < 4) rq.cBuffers = 4; if (rq.cbAlign < 1) rq.cbAlign = 1;
    memset(&ac, 0, sizeof ac);
    hr = IMemAllocator_SetProperties(al, &rq, &ac);
    if (FAILED(hr) || ac.cbBuffer < f->wfx->nBlockAlign) { logmsg("WavParse out: SetProperties %lx", hr); IMemAllocator_Release(al); IMemInputPin_Release(mip); IPin_Disconnect(recv); return FAILED(hr) ? hr : E_FAIL; }
    hr = IMemInputPin_NotifyAllocator(mip, al, FALSE);
    if (FAILED(hr)) { IMemAllocator_Release(al); IMemInputPin_Release(mip); IPin_Disconnect(recv); return hr; }
    f->chunk = ac.cbBuffer - ac.cbBuffer % f->wfx->nBlockAlign;
    IPin_AddRef(recv); f->outpeer = recv; f->mip = mip; f->alloc = al;
    logmsg("WavParse out: connected, %ld buffers of %ld", ac.cBuffers, ac.cbBuffer);
    return S_OK;
}
static HRESULT WINAPI OP_ReceiveConnection(IPin *t, IPin *c, const AM_MEDIA_TYPE *m) { return E_UNEXPECTED; }
static HRESULT WINAPI OP_Disconnect(IPin *t)
{
    WP *f = F_OUT(t);
    if (f->state != State_Stopped) return E_NOT_STOPPED;
    if (!f->outpeer) return S_FALSE;
    wp_stop_thread(f);
    if (f->alloc) { IMemAllocator_Release(f->alloc); f->alloc = NULL; }
    if (f->mip) { IMemInputPin_Release(f->mip); f->mip = NULL; }
    IPin_Release(f->outpeer); f->outpeer = NULL;
    return S_OK;
}
static HRESULT WINAPI OP_ConnectedTo(IPin *t, IPin **o)
{
    WP *f = F_OUT(t);
    if (!f->outpeer) { *o = NULL; return E_NOT_CONNECTED; }
    IPin_AddRef(f->outpeer); *o = f->outpeer; return S_OK;
}
static HRESULT WINAPI OP_ConnectionMediaType(IPin *t, AM_MEDIA_TYPE *m)
{
    WP *f = F_OUT(t);
    memset(m, 0, sizeof *m);
    if (!f->outpeer) return E_NOT_CONNECTED;
    return copy_mt(&f->mt, m);
}
static HRESULT WINAPI OP_QueryPinInfo(IPin *t, PIN_INFO *i)
{
    WP *f = F_OUT(t);
    i->pFilter = &f->bf; IBaseFilter_AddRef(i->pFilter);
    i->dir = PINDIR_OUTPUT; lstrcpyW(i->achName, L"Output"); return S_OK;
}
static HRESULT WINAPI OP_QueryDirection(IPin *t, PIN_DIRECTION *d) { *d = PINDIR_OUTPUT; return S_OK; }
static HRESULT WINAPI OP_QueryId(IPin *t, LPWSTR *id) { *id = CoTaskMemAlloc(14); lstrcpyW(*id, L"Output"); return S_OK; }
static HRESULT WINAPI OP_QueryAccept(IPin *t, const AM_MEDIA_TYPE *m)
{
    WP *f = F_OUT(t);
    return f->wfx && m && IsEqualGUID(&m->majortype, &G_MajAudio_) && IsEqualGUID(&m->subtype, &f->mt.subtype) ? S_OK : S_FALSE;
}
static HRESULT WINAPI OP_EnumMediaTypes(IPin *t, IEnumMediaTypes **o)
{
    WP *f = F_OUT(t); EMT *e = calloc(1, sizeof *e);
    e->e.lpVtbl = &emt_vtbl; e->ref = 1;
    if (f->wfx && SUCCEEDED(copy_mt(&f->mt, &e->mt))) e->have = 1;
    *o = &e->e; return S_OK;
}
static HRESULT WINAPI OP_QueryInternalConnections(IPin *t, IPin **p, ULONG *n)
{
    WP *f = F_OUT(t);
    if (!n || *n < 1) { if (n) *n = 1; return S_FALSE; }
    IPin_AddRef(&f->inpin); p[0] = &f->inpin; *n = 1; return S_OK;
}
static HRESULT WINAPI OP_EndOfStream(IPin *t) { return E_UNEXPECTED; }
static HRESULT WINAPI OP_BeginFlush(IPin *t) { return E_UNEXPECTED; }
static HRESULT WINAPI OP_EndFlush(IPin *t) { return E_UNEXPECTED; }
static HRESULT WINAPI OP_NewSegment(IPin *t, REFERENCE_TIME a, REFERENCE_TIME b, double r) { return E_UNEXPECTED; }
static const IPinVtbl op_vtbl = {OP_QI, OP_AddRef, OP_Release, OP_Connect, OP_ReceiveConnection, OP_Disconnect, OP_ConnectedTo,
    OP_ConnectionMediaType, OP_QueryPinInfo, OP_QueryDirection, OP_QueryId, OP_QueryAccept, OP_EnumMediaTypes,
    OP_QueryInternalConnections, OP_EndOfStream, OP_BeginFlush, OP_EndFlush, OP_NewSegment};

/* ---------- IEnumPins ---------- */
typedef struct { IEnumPins e; LONG ref; WP *f; int idx; } EP;
static HRESULT WINAPI EP_QI(IEnumPins *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IEnumPins)) { *o = t; ((EP *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI EP_AddRef(IEnumPins *t) { return ++((EP *)t)->ref; }
static ULONG WINAPI EP_Release(IEnumPins *t) { EP *e = (EP *)t; if (!--e->ref) { IBaseFilter_Release(&e->f->bf); free(e); return 0; } return e->ref; }
static HRESULT WINAPI EP_Next(IEnumPins *t, ULONG n, IPin **pins, ULONG *got)
{
    EP *e = (EP *)t; ULONG k = 0;
    while (k < n && e->idx < 2) { IPin *p = e->idx == 0 ? &e->f->inpin : &e->f->outpin; IPin_AddRef(p); pins[k++] = p; e->idx++; }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI EP_Skip(IEnumPins *t, ULONG n) { EP *e = (EP *)t; e->idx += n; return e->idx > 2 ? S_FALSE : S_OK; }
static HRESULT WINAPI EP_Reset(IEnumPins *t) { ((EP *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI EP_Clone(IEnumPins *t, IEnumPins **o) { EP *e = (EP *)t, *n = calloc(1, sizeof *n); *n = *e; n->ref = 1; IBaseFilter_AddRef(&n->f->bf); *o = &n->e; return S_OK; }
static const IEnumPinsVtbl ep_vtbl = {EP_QI, EP_AddRef, EP_Release, EP_Next, EP_Skip, EP_Reset, EP_Clone};

/* ---------- IBaseFilter ---------- */
static HRESULT WINAPI BF_QI(IBaseFilter *t, REFIID r, void **o)
{
    WP *f = F_BF(t);
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IPersist) || IsEqualGUID(r, &IID_IMediaFilter) || IsEqualGUID(r, &IID_IBaseFilter)) *o = &f->bf;
    else if (IsEqualGUID(r, &IID_IMediaSeeking)) *o = &f->ms;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&f->ref);
    return S_OK;
}
static ULONG WINAPI BF_AddRef(IBaseFilter *t) { return InterlockedIncrement(&F_BF(t)->ref); }
static ULONG WINAPI BF_Release(IBaseFilter *t)
{
    WP *f = F_BF(t);
    LONG n = InterlockedDecrement(&f->ref);
    if (!n) {
        wp_stop_thread(f);
        if (f->alloc) IMemAllocator_Release(f->alloc);
        if (f->mip) IMemInputPin_Release(f->mip);
        if (f->outpeer) IPin_Release(f->outpeer);
        if (f->inpeer) IPin_Release(f->inpeer);
        if (f->rd) IAsyncReader_Release(f->rd);
        if (f->wfx) CoTaskMemFree(f->wfx);
        free_mt(&f->mt);
        CloseHandle(f->ev_run); CloseHandle(f->ev_stop);
        DeleteCriticalSection(&f->lock);
        free(f);
    }
    return n;
}
static HRESULT WINAPI BF_GetClassID(IBaseFilter *t, CLSID *c) { *c = CLSID_WavParse_; return S_OK; }
static HRESULT WINAPI BF_Stop(IBaseFilter *t)
{
    WP *f = F_BF(t);
    logmsg("WavParse Stop");
    wp_stop_thread(f);
    EnterCriticalSection(&f->lock); f->state = State_Stopped; LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_Pause(IBaseFilter *t)
{
    WP *f = F_BF(t); int was;
    logmsg("WavParse Pause (state %d)", f->state);
    EnterCriticalSection(&f->lock); was = f->state; f->state = State_Paused; LeaveCriticalSection(&f->lock);
    if (was == State_Stopped) { f->curb = f->startb; wp_start_thread(f); }
    return S_OK;
}
static HRESULT WINAPI BF_Run(IBaseFilter *t, REFERENCE_TIME s)
{
    WP *f = F_BF(t); int was;
    logmsg("WavParse Run (state %d)", f->state);
    EnterCriticalSection(&f->lock); was = f->state; f->state = State_Running; LeaveCriticalSection(&f->lock);
    if (was == State_Stopped) { f->curb = f->startb; wp_start_thread(f); }
    SetEvent(f->ev_run);
    return S_OK;
}
static HRESULT WINAPI BF_GetState(IBaseFilter *t, DWORD ms, FILTER_STATE *s) { *s = F_BF(t)->state; return S_OK; }
static HRESULT WINAPI BF_SetSyncSource(IBaseFilter *t, IReferenceClock *c) { return S_OK; }
static HRESULT WINAPI BF_GetSyncSource(IBaseFilter *t, IReferenceClock **c) { *c = NULL; return S_OK; }
static HRESULT WINAPI BF_EnumPins(IBaseFilter *t, IEnumPins **o)
{
    EP *e = calloc(1, sizeof *e);
    e->e.lpVtbl = &ep_vtbl; e->ref = 1; e->f = F_BF(t); IBaseFilter_AddRef(t);
    *o = &e->e; return S_OK;
}
static HRESULT WINAPI BF_FindPin(IBaseFilter *t, LPCWSTR id, IPin **o)
{
    WP *f = F_BF(t);
    if (!lstrcmpiW(id, L"input")) { IPin_AddRef(&f->inpin); *o = &f->inpin; return S_OK; }
    if (!lstrcmpiW(id, L"Output")) { IPin_AddRef(&f->outpin); *o = &f->outpin; return S_OK; }
    *o = NULL; return E_NOT_FOUND;
}
static HRESULT WINAPI BF_QueryFilterInfo(IBaseFilter *t, FILTER_INFO *i)
{
    WP *f = F_BF(t);
    lstrcpyW(i->achName, f->gname);
    i->pGraph = f->graph; if (f->graph) IFilterGraph_AddRef(f->graph);
    return S_OK;
}
static HRESULT WINAPI BF_JoinFilterGraph(IBaseFilter *t, IFilterGraph *g, LPCWSTR n)
{
    WP *f = F_BF(t);
    f->graph = g;
    if (n) lstrcpynW(f->gname, n, 128); else f->gname[0] = 0;
    return S_OK;
}
static HRESULT WINAPI BF_QueryVendorInfo(IBaseFilter *t, LPWSTR *v) { return E_NOTIMPL; }
static const IBaseFilterVtbl bf_vtbl = {BF_QI, BF_AddRef, BF_Release, BF_GetClassID, BF_Stop, BF_Pause, BF_Run, BF_GetState,
    BF_SetSyncSource, BF_GetSyncSource, BF_EnumPins, BF_FindPin, BF_QueryFilterInfo, BF_JoinFilterGraph, BF_QueryVendorInfo};

/* ---------- class factory ---------- */
typedef struct { IClassFactory cf; } CF;
static HRESULT WINAPI CF_QI(IClassFactory *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IClassFactory)) { *o = t; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI CF_AddRef(IClassFactory *t) { return 2; }
static ULONG WINAPI CF_Release(IClassFactory *t) { return 1; }
static HRESULT WINAPI CF_Create(IClassFactory *t, IUnknown *outer, REFIID r, void **o)
{
    logmsg("WavParse CreateInstance(%s)", guidstr(r));
    if (outer) return CLASS_E_NOAGGREGATION;
    WP *f = calloc(1, sizeof *f);
    f->bf.lpVtbl = &bf_vtbl; f->inpin.lpVtbl = &ip_vtbl; f->outpin.lpVtbl = &op_vtbl; f->ms.lpVtbl = &ms_vtbl;
    f->ref = 1; f->state = State_Stopped;
    InitializeCriticalSection(&f->lock);
    f->ev_run = CreateEventA(NULL, TRUE, FALSE, NULL); f->ev_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    HRESULT hr = BF_QI(&f->bf, r, o);
    BF_Release(&f->bf);
    return hr;
}
static HRESULT WINAPI CF_Lock(IClassFactory *t, BOOL l) { return S_OK; }
static const IClassFactoryVtbl cf_vtbl = {CF_QI, CF_AddRef, CF_Release, CF_Create, CF_Lock};
static CF the_cf = {{&cf_vtbl}};

int wavparse_clsid(REFCLSID c) { return IsEqualGUID(c, &CLSID_WavParse_); }
HRESULT wavparse_get_factory(REFIID r, void **o) { return CF_QI(&the_cf.cf, r, o); }
