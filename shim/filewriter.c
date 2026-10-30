/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DirectShow "File Writer" (CLSID_FileWriter) replacement.
 * Wine's built-in one has no IStream on its input pin. The game's own "Wave Dest" filter (a copy of the DirectShow
 * WavDest sample) writes the RIFF header at stop by QueryInterface(IID_IStream) on the downstream pin followed by
 * Seek(0)+Write(); without IStream the header stays zeroed and the audio pre-render of an export is unusable. */
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

static const CLSID CLSID_FileWriter_ = {0x8596e5f0, 0x0da5, 0x11d0, {0xbd, 0x21, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86}};
static const GUID G_MemAllocClsid_ = {0x1e651cc0, 0xb199, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
static const GUID G_MiscFlags_ = {0x2dd74950, 0xa890, 0x11d1, {0xab, 0xe8, 0x00, 0xa0, 0xc9, 0x05, 0xf3, 0x75}};
static const GUID G_IUnk_ = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
#define E_NOT_CONNECTED ((HRESULT)0x80040209)
#define E_NOT_ACCEPTED ((HRESULT)0x8004022A)
#define E_NOT_FOUND ((HRESULT)0x80040216)
#define E_ALREADY_CONN ((HRESULT)0x80040204)
#define E_NOT_STOPPED ((HRESULT)0x80040225)

typedef struct FW FW;
struct FW {
    IBaseFilter bf;
    IFileSinkFilter2 fs;
    IAMFilterMiscFlags mf;
    IPin pin;
    IMemInputPin mip;
    IStream st;
    LONG ref;
    CRITICAL_SECTION lock;
    FILTER_STATE state;
    IFilterGraph *graph; WCHAR gname[128];
    WCHAR *file; DWORD mode;
    IPin *peer;
    AM_MEDIA_TYPE mt;
    HANDLE h;
    LONGLONG seqpos, strpos;
    long nrecv;
};
#define F_BF(p) ((FW *)(p))
#define F_FS(p) ((FW *)((BYTE *)(p) - offsetof(FW, fs)))
#define F_MF(p) ((FW *)((BYTE *)(p) - offsetof(FW, mf)))
#define F_PIN(p) ((FW *)((BYTE *)(p) - offsetof(FW, pin)))
#define F_MIP(p) ((FW *)((BYTE *)(p) - offsetof(FW, mip)))
#define F_ST(p) ((FW *)((BYTE *)(p) - offsetof(FW, st)))

static int fw_valid(FW *f) { return f->h && f->h != INVALID_HANDLE_VALUE; }
/* open (CREATE_ALWAYS) once; later runs truncate the already-open handle. Caller holds the lock. */
static void fw_open(FW *f)
{
    if (!f->file) return;
    if (fw_valid(f)) {
        LARGE_INTEGER z = {{0, 0}};
        SetFilePointerEx(f->h, z, NULL, FILE_BEGIN); SetEndOfFile(f->h);
    } else {
        f->h = CreateFileW(f->file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        logmsg("FileWriter open %ls -> %s", f->file, fw_valid(f) ? "ok" : "FAILED");
    }
    f->seqpos = f->strpos = 0;
}
static HRESULT fw_write_at(FW *f, LONGLONG pos, const void *buf, DWORD n, DWORD *done)
{
    LARGE_INTEGER li; DWORD w = 0;
    if (!fw_valid(f)) { f->h = CreateFileW(f->file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); }
    if (!fw_valid(f)) return E_FAIL;
    li.QuadPart = pos;
    if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN)) return E_FAIL;
    if (n && !WriteFile(f->h, buf, n, &w, NULL)) return E_FAIL;
    if (done) *done = w;
    return S_OK;
}

/* ---------- IStream (input pin) ---------- */
static HRESULT WINAPI WP_QI(IPin *t, REFIID r, void **o);
static HRESULT WINAPI ST_QI(IStream *t, REFIID r, void **o) { return WP_QI(&F_ST(t)->pin, r, o); }
static ULONG WINAPI ST_AddRef(IStream *t) { return IBaseFilter_AddRef(&F_ST(t)->bf); }
static ULONG WINAPI ST_Release(IStream *t) { return IBaseFilter_Release(&F_ST(t)->bf); }
static HRESULT WINAPI ST_Read(IStream *t, void *p, ULONG n, ULONG *got) { if (got) *got = 0; return E_NOTIMPL; }
static HRESULT WINAPI ST_Write(IStream *t, const void *p, ULONG n, ULONG *done)
{
    FW *f = F_ST(t); DWORD w = 0; HRESULT hr;
    EnterCriticalSection(&f->lock);
    hr = fw_write_at(f, f->strpos, p, n, &w);
    f->strpos += w;
    LeaveCriticalSection(&f->lock);
    logmsg("FileWriter IStream::Write(pos=%lld n=%lu) -> %lx", (long long)(f->strpos - w), n, hr);
    if (done) *done = w;
    return hr;
}
static HRESULT WINAPI ST_Seek(IStream *t, LARGE_INTEGER mv, DWORD org, ULARGE_INTEGER *np)
{
    FW *f = F_ST(t); LONGLONG base = 0;
    EnterCriticalSection(&f->lock);
    if (org == STREAM_SEEK_CUR) base = f->strpos;
    else if (org == STREAM_SEEK_END) { LARGE_INTEGER sz = {{0, 0}}; if (fw_valid(f) && GetFileSizeEx(f->h, &sz)) base = sz.QuadPart; }
    else if (org != STREAM_SEEK_SET) { LeaveCriticalSection(&f->lock); return STG_E_INVALIDFUNCTION; }
    f->strpos = base + mv.QuadPart;
    if (f->strpos < 0) f->strpos = 0;
    if (np) np->QuadPart = f->strpos;
    LeaveCriticalSection(&f->lock);
    logmsg("FileWriter IStream::Seek(%lld, %lu) -> %lld", (long long)mv.QuadPart, org, (long long)f->strpos);
    return S_OK;
}
static HRESULT WINAPI ST_SetSize(IStream *t, ULARGE_INTEGER n)
{
    FW *f = F_ST(t); LARGE_INTEGER li; HRESULT hr = S_OK;
    EnterCriticalSection(&f->lock);
    if (fw_valid(f)) { li.QuadPart = (LONGLONG)n.QuadPart; if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN) || !SetEndOfFile(f->h)) hr = E_FAIL; } else hr = E_FAIL;
    LeaveCriticalSection(&f->lock);
    return hr;
}
static HRESULT WINAPI ST_CopyTo(IStream *t, IStream *d, ULARGE_INTEGER n, ULARGE_INTEGER *r, ULARGE_INTEGER *w) { return E_NOTIMPL; }
static HRESULT WINAPI ST_Commit(IStream *t, DWORD fl) { FW *f = F_ST(t); if (fw_valid(f)) FlushFileBuffers(f->h); return S_OK; }
static HRESULT WINAPI ST_Revert(IStream *t) { return E_NOTIMPL; }
static HRESULT WINAPI ST_Lock(IStream *t, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD ty) { return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI ST_Unlock(IStream *t, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD ty) { return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI ST_Stat(IStream *t, STATSTG *s, DWORD fl)
{
    FW *f = F_ST(t); LARGE_INTEGER sz = {{0, 0}};
    memset(s, 0, sizeof *s);
    if (fw_valid(f)) GetFileSizeEx(f->h, &sz);
    s->type = STGTY_STREAM; s->cbSize.QuadPart = sz.QuadPart;
    return S_OK;
}
static HRESULT WINAPI ST_Clone(IStream *t, IStream **o) { *o = NULL; return E_NOTIMPL; }
static const IStreamVtbl st_vtbl = {ST_QI, ST_AddRef, ST_Release, ST_Read, ST_Write, ST_Seek, ST_SetSize, ST_CopyTo, ST_Commit,
    ST_Revert, ST_Lock, ST_Unlock, ST_Stat, ST_Clone};

/* ---------- input pin ---------- */
static HRESULT WINAPI WP_QI(IPin *t, REFIID r, void **o)
{
    FW *f = F_PIN(t);
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IPin)) *o = &f->pin;
    else if (IsEqualGUID(r, &IID_IMemInputPin)) *o = &f->mip;
    else if (IsEqualGUID(r, &IID_IStream) || IsEqualGUID(r, &IID_ISequentialStream)) *o = &f->st;
    else { logmsg("FileWriterPin QI %s -> E_NOINTERFACE", guidstr(r)); *o = NULL; return E_NOINTERFACE; }
    IBaseFilter_AddRef(&f->bf);
    return S_OK;
}
static ULONG WINAPI WP_AddRef(IPin *t) { return IBaseFilter_AddRef(&F_PIN(t)->bf); }
static ULONG WINAPI WP_Release(IPin *t) { return IBaseFilter_Release(&F_PIN(t)->bf); }
static HRESULT WINAPI WP_Connect(IPin *t, IPin *r, const AM_MEDIA_TYPE *m) { return E_UNEXPECTED; }
static void free_mt(AM_MEDIA_TYPE *m) { if (m->pbFormat) CoTaskMemFree(m->pbFormat); memset(m, 0, sizeof *m); }
static HRESULT WINAPI WP_ReceiveConnection(IPin *t, IPin *conn, const AM_MEDIA_TYPE *m)
{
    FW *f = F_PIN(t);
    logmsg("FileWriter pin: ReceiveConnection major=%s sub=%s", m ? guidstr(&m->majortype) : "-", m ? guidstr(&m->subtype) : "-");
    if (f->peer) return E_ALREADY_CONN;
    if (f->state != State_Stopped) return E_NOT_STOPPED;
    free_mt(&f->mt);
    if (m) {
        f->mt = *m; f->mt.pUnk = NULL; f->mt.pbFormat = NULL;
        if (m->cbFormat && m->pbFormat) { f->mt.pbFormat = CoTaskMemAlloc(m->cbFormat); memcpy(f->mt.pbFormat, m->pbFormat, m->cbFormat); }
    }
    IPin_AddRef(conn); f->peer = conn;
    return S_OK;
}
static HRESULT WINAPI WP_Disconnect(IPin *t)
{
    FW *f = F_PIN(t);
    logmsg("FileWriter pin: Disconnect");
    if (f->state != State_Stopped) return E_NOT_STOPPED;
    if (!f->peer) return S_FALSE;
    IPin_Release(f->peer); f->peer = NULL; return S_OK;
}
static HRESULT WINAPI WP_ConnectedTo(IPin *t, IPin **o)
{
    FW *f = F_PIN(t);
    if (!f->peer) { *o = NULL; return E_NOT_CONNECTED; }
    IPin_AddRef(f->peer); *o = f->peer; return S_OK;
}
static HRESULT WINAPI WP_ConnectionMediaType(IPin *t, AM_MEDIA_TYPE *m)
{
    FW *f = F_PIN(t);
    memset(m, 0, sizeof *m);
    if (!f->peer) return E_NOT_CONNECTED;
    *m = f->mt; m->pbFormat = NULL; m->pUnk = NULL;
    if (f->mt.cbFormat && f->mt.pbFormat) { m->pbFormat = CoTaskMemAlloc(f->mt.cbFormat); memcpy(m->pbFormat, f->mt.pbFormat, f->mt.cbFormat); }
    return S_OK;
}
static HRESULT WINAPI WP_QueryPinInfo(IPin *t, PIN_INFO *i)
{
    FW *f = F_PIN(t);
    i->pFilter = &f->bf; IBaseFilter_AddRef(i->pFilter);
    i->dir = PINDIR_INPUT; lstrcpyW(i->achName, L"in"); return S_OK;
}
static HRESULT WINAPI WP_QueryDirection(IPin *t, PIN_DIRECTION *d) { *d = PINDIR_INPUT; return S_OK; }
static HRESULT WINAPI WP_QueryId(IPin *t, LPWSTR *id) { *id = CoTaskMemAlloc(6); lstrcpyW(*id, L"in"); return S_OK; }
static HRESULT WINAPI WP_QueryAccept(IPin *t, const AM_MEDIA_TYPE *m) { return S_OK; }
typedef struct { IEnumMediaTypes e; LONG ref; } EMT;
static HRESULT WINAPI E_QI(IEnumMediaTypes *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IEnumMediaTypes)) { *o = t; ((EMT *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI E_AddRef(IEnumMediaTypes *t) { return ++((EMT *)t)->ref; }
static ULONG WINAPI E_Release(IEnumMediaTypes *t) { EMT *e = (EMT *)t; if (!--e->ref) { free(e); return 0; } return e->ref; }
static HRESULT WINAPI E_Next(IEnumMediaTypes *t, ULONG n, AM_MEDIA_TYPE **m, ULONG *g) { if (g) *g = 0; return S_FALSE; }
static HRESULT WINAPI E_Skip(IEnumMediaTypes *t, ULONG n) { return S_FALSE; }
static HRESULT WINAPI E_Reset(IEnumMediaTypes *t) { return S_OK; }
static HRESULT WINAPI E_Clone(IEnumMediaTypes *t, IEnumMediaTypes **o) { EMT *e = calloc(1, sizeof *e); e->e.lpVtbl = t->lpVtbl; e->ref = 1; *o = &e->e; return S_OK; }
static const IEnumMediaTypesVtbl emt_vtbl = {E_QI, E_AddRef, E_Release, E_Next, E_Skip, E_Reset, E_Clone};
static HRESULT WINAPI WP_EnumMediaTypes(IPin *t, IEnumMediaTypes **o) { EMT *e = calloc(1, sizeof *e); e->e.lpVtbl = &emt_vtbl; e->ref = 1; *o = &e->e; return S_OK; }
static HRESULT WINAPI WP_QueryInternalConnections(IPin *t, IPin **p, ULONG *n) { return E_NOTIMPL; }
static HRESULT WINAPI WP_EndOfStream(IPin *t)
{
    FW *f = F_PIN(t);
    IMediaEventSink *sink = NULL;
    logmsg("FileWriter pin: EndOfStream (%ld samples, %lld bytes)", f->nrecv, (long long)f->seqpos);
    EnterCriticalSection(&f->lock);
    if (fw_valid(f)) FlushFileBuffers(f->h);
    LeaveCriticalSection(&f->lock);
    if (f->graph && SUCCEEDED(IFilterGraph_QueryInterface(f->graph, &IID_IMediaEventSink, (void **)&sink))) {
        IMediaEventSink_Notify(sink, EC_COMPLETE, S_OK, (LONG_PTR)&f->bf);
        IMediaEventSink_Release(sink);
    }
    return S_OK;
}
static HRESULT WINAPI WP_BeginFlush(IPin *t) { return S_OK; }
static HRESULT WINAPI WP_EndFlush(IPin *t) { return S_OK; }
static HRESULT WINAPI WP_NewSegment(IPin *t, REFERENCE_TIME a, REFERENCE_TIME b, double r) { return S_OK; }
static const IPinVtbl wp_vtbl = {WP_QI, WP_AddRef, WP_Release, WP_Connect, WP_ReceiveConnection, WP_Disconnect, WP_ConnectedTo,
    WP_ConnectionMediaType, WP_QueryPinInfo, WP_QueryDirection, WP_QueryId, WP_QueryAccept, WP_EnumMediaTypes,
    WP_QueryInternalConnections, WP_EndOfStream, WP_BeginFlush, WP_EndFlush, WP_NewSegment};

/* ---------- IMemInputPin ---------- */
static HRESULT WINAPI MP_QI(IMemInputPin *t, REFIID r, void **o) { return WP_QI(&F_MIP(t)->pin, r, o); }
static ULONG WINAPI MP_AddRef(IMemInputPin *t) { return IBaseFilter_AddRef(&F_MIP(t)->bf); }
static ULONG WINAPI MP_Release(IMemInputPin *t) { return IBaseFilter_Release(&F_MIP(t)->bf); }
static HRESULT WINAPI MP_GetAllocator(IMemInputPin *t, IMemAllocator **o) { return CoCreateInstance(&G_MemAllocClsid_, NULL, CLSCTX_INPROC_SERVER, &IID_IMemAllocator, (void **)o); }
static HRESULT WINAPI MP_NotifyAllocator(IMemInputPin *t, IMemAllocator *a, BOOL ro) { return S_OK; }
static HRESULT WINAPI MP_GetAllocatorRequirements(IMemInputPin *t, ALLOCATOR_PROPERTIES *p) { return E_NOTIMPL; }
static HRESULT WINAPI MP_Receive(IMemInputPin *t, IMediaSample *ms)
{
    FW *f = F_MIP(t); BYTE *buf = NULL; LONG len; DWORD w = 0; HRESULT hr;
    len = IMediaSample_GetActualDataLength(ms);
    IMediaSample_GetPointer(ms, &buf);
    EnterCriticalSection(&f->lock);
    if (!fw_valid(f)) fw_open(f);
    hr = fw_write_at(f, f->seqpos, buf, len, &w);
    f->seqpos += w; f->nrecv++;
    LeaveCriticalSection(&f->lock);
    if (f->nrecv <= 3) logmsg("FileWriter Receive #%ld len=%ld -> %lx (pos now %lld)", f->nrecv, len, hr, (long long)f->seqpos);
    return hr;
}
static HRESULT WINAPI MP_ReceiveMultiple(IMemInputPin *t, IMediaSample **s, LONG n, LONG *done)
{
    LONG i; HRESULT hr = S_OK;
    for (i = 0; i < n && hr == S_OK; i++) hr = MP_Receive(t, s[i]);
    *done = i; return hr;
}
static HRESULT WINAPI MP_ReceiveCanBlock(IMemInputPin *t) { return S_FALSE; }
static const IMemInputPinVtbl mp_vtbl = {MP_QI, MP_AddRef, MP_Release, MP_GetAllocator, MP_NotifyAllocator, MP_GetAllocatorRequirements,
    MP_Receive, MP_ReceiveMultiple, MP_ReceiveCanBlock};

/* ---------- IEnumPins ---------- */
typedef struct { IEnumPins e; LONG ref; FW *f; int idx; } EP;
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
    while (k < n && e->idx < 1) { IPin_AddRef(&e->f->pin); pins[k++] = &e->f->pin; e->idx++; }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI EP_Skip(IEnumPins *t, ULONG n) { EP *e = (EP *)t; e->idx += n; return e->idx > 1 ? S_FALSE : S_OK; }
static HRESULT WINAPI EP_Reset(IEnumPins *t) { ((EP *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI EP_Clone(IEnumPins *t, IEnumPins **o) { EP *e = (EP *)t, *n = calloc(1, sizeof *n); *n = *e; n->ref = 1; IBaseFilter_AddRef(&n->f->bf); *o = &n->e; return S_OK; }
static const IEnumPinsVtbl ep_vtbl = {EP_QI, EP_AddRef, EP_Release, EP_Next, EP_Skip, EP_Reset, EP_Clone};

/* ---------- IBaseFilter ---------- */
static HRESULT WINAPI BF_QI(IBaseFilter *t, REFIID r, void **o)
{
    FW *f = F_BF(t);
    if (IsEqualGUID(r, &G_IUnk_) || IsEqualGUID(r, &IID_IPersist) || IsEqualGUID(r, &IID_IMediaFilter) || IsEqualGUID(r, &IID_IBaseFilter)) *o = &f->bf;
    else if (IsEqualGUID(r, &IID_IFileSinkFilter) || IsEqualGUID(r, &IID_IFileSinkFilter2)) *o = &f->fs;
    else if (IsEqualGUID(r, &G_MiscFlags_)) *o = &f->mf;
    else { logmsg("FileWriter QI %s -> E_NOINTERFACE", guidstr(r)); *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&f->ref);
    return S_OK;
}
static ULONG WINAPI BF_AddRef(IBaseFilter *t) { return InterlockedIncrement(&F_BF(t)->ref); }
static ULONG WINAPI BF_Release(IBaseFilter *t)
{
    FW *f = F_BF(t);
    LONG n = InterlockedDecrement(&f->ref);
    if (!n) {
        logmsg("FileWriter destroyed");
        if (fw_valid(f)) CloseHandle(f->h);
        if (f->peer) IPin_Release(f->peer);
        free_mt(&f->mt);
        free(f->file);
        DeleteCriticalSection(&f->lock);
        free(f);
    }
    return n;
}
static HRESULT WINAPI BF_GetClassID(IBaseFilter *t, CLSID *c) { *c = CLSID_FileWriter_; return S_OK; }
static HRESULT WINAPI BF_Stop(IBaseFilter *t)
{
    FW *f = F_BF(t);
    logmsg("FileWriter Stop");
    EnterCriticalSection(&f->lock);
    f->state = State_Stopped;
    if (fw_valid(f)) FlushFileBuffers(f->h); /* handle stays open: the upstream filter may still need IStream after we stop */
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_Pause(IBaseFilter *t)
{
    FW *f = F_BF(t);
    logmsg("FileWriter Pause (state %d)", f->state);
    EnterCriticalSection(&f->lock);
    if (f->state == State_Stopped) { fw_open(f); f->nrecv = 0; }
    f->state = State_Paused;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI BF_Run(IBaseFilter *t, REFERENCE_TIME s)
{
    FW *f = F_BF(t);
    logmsg("FileWriter Run (state %d)", f->state);
    EnterCriticalSection(&f->lock);
    if (f->state == State_Stopped) { fw_open(f); f->nrecv = 0; }
    f->state = State_Running;
    LeaveCriticalSection(&f->lock);
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
    FW *f = F_BF(t);
    if (!lstrcmpiW(id, L"in")) { IPin_AddRef(&f->pin); *o = &f->pin; return S_OK; }
    *o = NULL; return E_NOT_FOUND;
}
static HRESULT WINAPI BF_QueryFilterInfo(IBaseFilter *t, FILTER_INFO *i)
{
    FW *f = F_BF(t);
    lstrcpyW(i->achName, f->gname);
    i->pGraph = f->graph; if (f->graph) IFilterGraph_AddRef(f->graph);
    return S_OK;
}
static HRESULT WINAPI BF_JoinFilterGraph(IBaseFilter *t, IFilterGraph *g, LPCWSTR n)
{
    FW *f = F_BF(t);
    f->graph = g;
    if (n) lstrcpynW(f->gname, n, 128); else f->gname[0] = 0;
    return S_OK;
}
static HRESULT WINAPI BF_QueryVendorInfo(IBaseFilter *t, LPWSTR *v) { return E_NOTIMPL; }
static const IBaseFilterVtbl bf_vtbl = {BF_QI, BF_AddRef, BF_Release, BF_GetClassID, BF_Stop, BF_Pause, BF_Run, BF_GetState,
    BF_SetSyncSource, BF_GetSyncSource, BF_EnumPins, BF_FindPin, BF_QueryFilterInfo, BF_JoinFilterGraph, BF_QueryVendorInfo};

/* IFileSinkFilter2 */
static HRESULT WINAPI FS_QI(IFileSinkFilter2 *t, REFIID r, void **o) { return BF_QI(&F_FS(t)->bf, r, o); }
static ULONG WINAPI FS_AddRef(IFileSinkFilter2 *t) { return BF_AddRef(&F_FS(t)->bf); }
static ULONG WINAPI FS_Release(IFileSinkFilter2 *t) { return BF_Release(&F_FS(t)->bf); }
static HRESULT WINAPI FS_SetFileName(IFileSinkFilter2 *t, LPCOLESTR fn, const AM_MEDIA_TYPE *m)
{
    FW *f = F_FS(t);
    logmsg("FileWriter SetFileName(%ls)", fn);
    EnterCriticalSection(&f->lock);
    if (fw_valid(f)) { CloseHandle(f->h); f->h = INVALID_HANDLE_VALUE; }
    free(f->file); f->file = _wcsdup(fn);
    LeaveCriticalSection(&f->lock);
    return S_OK;
}
static HRESULT WINAPI FS_GetCurFile(IFileSinkFilter2 *t, LPOLESTR *fn, AM_MEDIA_TYPE *m)
{
    FW *f = F_FS(t);
    if (f->file) { *fn = CoTaskMemAlloc((lstrlenW(f->file) + 1) * 2); lstrcpyW(*fn, f->file); } else *fn = NULL;
    if (m) memset(m, 0, sizeof *m);
    return S_OK;
}
static HRESULT WINAPI FS_SetMode(IFileSinkFilter2 *t, DWORD m) { F_FS(t)->mode = m; return S_OK; }
static HRESULT WINAPI FS_GetMode(IFileSinkFilter2 *t, DWORD *m) { *m = F_FS(t)->mode; return S_OK; }
static const IFileSinkFilter2Vtbl fs_vtbl = {FS_QI, FS_AddRef, FS_Release, FS_SetFileName, FS_GetCurFile, FS_SetMode, FS_GetMode};

/* IAMFilterMiscFlags */
static HRESULT WINAPI MF_QI(IAMFilterMiscFlags *t, REFIID r, void **o) { return BF_QI(&F_MF(t)->bf, r, o); }
static ULONG WINAPI MF_AddRef(IAMFilterMiscFlags *t) { return BF_AddRef(&F_MF(t)->bf); }
static ULONG WINAPI MF_Release(IAMFilterMiscFlags *t) { return BF_Release(&F_MF(t)->bf); }
static ULONG WINAPI MF_Get(IAMFilterMiscFlags *t) { return AM_FILTER_MISC_FLAGS_IS_RENDERER; }
static const IAMFilterMiscFlagsVtbl mf_vtbl = {MF_QI, MF_AddRef, MF_Release, MF_Get};

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
    logmsg("FileWriter CreateInstance(%s)", guidstr(r));
    if (outer) return CLASS_E_NOAGGREGATION;
    FW *f = calloc(1, sizeof *f);
    f->bf.lpVtbl = &bf_vtbl; f->fs.lpVtbl = &fs_vtbl; f->mf.lpVtbl = &mf_vtbl; f->pin.lpVtbl = &wp_vtbl;
    f->mip.lpVtbl = &mp_vtbl; f->st.lpVtbl = &st_vtbl; f->ref = 1; f->h = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&f->lock);
    f->state = State_Stopped; f->mode = 1;
    HRESULT hr = BF_QI(&f->bf, r, o);
    BF_Release(&f->bf);
    return hr;
}
static HRESULT WINAPI CF_Lock(IClassFactory *t, BOOL l) { return S_OK; }
static const IClassFactoryVtbl cf_vtbl = {CF_QI, CF_AddRef, CF_Release, CF_Create, CF_Lock};
static CF the_cf = {{&cf_vtbl}};

int filewriter_clsid(REFCLSID c) { return IsEqualGUID(c, &CLSID_FileWriter_); }
HRESULT filewriter_get_factory(REFIID r, void **o) { return CF_QI(&the_cf.cf, r, o); }
