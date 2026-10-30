/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CLSID_SystemDeviceEnum override + "Audio Enc" pass-through filter.
 *
 * The game's WMV-with-audio export builds   File Reader for WAV -> Wav Parser -> "Audio Enc" -> ASF Enc/Writer
 * where "Audio Enc" is looked up by FriendlyName ("Microsoft ADPCM" by default) in the DirectShow audio compressor
 * category. Wine's category is empty, the lookup fails and the export silently aborts back to the export screen.
 * We list one "Microsoft ADPCM" entry in that category (all other categories are forwarded to Wine's devenum) whose
 * filter simply passes the PCM samples through unchanged to our ASF writer, which does the WMA encoding itself. */
#define COBJMACROS
#define STRSAFE_NO_DEPRECATE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <dshow.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

void logmsg(const char *fmt, ...);
const char *guidstr(const GUID *g);

static const CLSID CLSID_SysDevEnum_ = {0x62BE5D10, 0x60EB, 0x11d0, {0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86}};
static const GUID CAT_AudioCompressor = {0x33D9A761, 0x90C8, 0x11d0, {0xBD, 0x43, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86}};
static const CLSID CLSID_AudioEncPass = {0x6d1e0c10, 0x5a77, 0x4f0e, {0x9b, 0x21, 0x3c, 0x44, 0x1d, 0x7e, 0x02, 0xa1}};
static const GUID G_MemAlloc = {0x1e651cc0, 0xb199, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
static const GUID G_Unk = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID G_Audio_ = {0x73647561, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_PCM_ = {0x00000001, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_FmtWave_ = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
#define FAUDIO_NAME L"Microsoft ADPCM"
#define E_NOT_CONNECTED_ ((HRESULT)0x80040209)
#define E_NOT_ACCEPTED_ ((HRESULT)0x8004022A)
#define E_NOT_FOUND_ ((HRESULT)0x80040216)
#define E_ALREADY_CONN_ ((HRESULT)0x80040204)
#define E_NOT_STOPPED_ ((HRESULT)0x80040225)

/* =========================== pass-through filter =========================== */
typedef struct AE AE;
typedef struct APin {
    IPin pin;
    IMemInputPin mip; /* input pin only */
    AE *f;
    int out; /* 1 = output pin */
    IPin *peer;
    IMemInputPin *peer_mip; /* output pin: downstream */
    IMemAllocator *alloc;   /* input pin: allocator from upstream */
    AM_MEDIA_TYPE mt;
} APin;
struct AE {
    IBaseFilter bf;
    LONG ref;
    FILTER_STATE state;
    IFilterGraph *graph; WCHAR gname[128];
    APin in, out;
};
#define A_BF(p) ((AE *)(p))
#define A_PIN(p) ((APin *)(p))
#define A_MIP(p) ((APin *)((BYTE *)(p) - offsetof(APin, mip)))

static void mt_free(AM_MEDIA_TYPE *m) { if (m->pbFormat) CoTaskMemFree(m->pbFormat); memset(m, 0, sizeof *m); }
static void mt_copy(AM_MEDIA_TYPE *d, const AM_MEDIA_TYPE *s)
{
    *d = *s; d->pUnk = NULL; d->pbFormat = NULL;
    if (s->cbFormat && s->pbFormat) { d->pbFormat = CoTaskMemAlloc(s->cbFormat); memcpy(d->pbFormat, s->pbFormat, s->cbFormat); }
}
static int pcm_ok(const AM_MEDIA_TYPE *m)
{
    if (!m) return 0;
    if (!IsEqualGUID(&m->majortype, &G_Audio_) || !IsEqualGUID(&m->subtype, &G_PCM_) || !IsEqualGUID(&m->formattype, &G_FmtWave_)) return 0;
    return m->cbFormat >= sizeof(WAVEFORMATEX) && m->pbFormat;
}

static HRESULT WINAPI AP_QI(IPin *t, REFIID r, void **o)
{
    APin *p = A_PIN(t);
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IPin)) *o = &p->pin;
    else if (!p->out && IsEqualGUID(r, &IID_IMemInputPin)) *o = &p->mip;
    else { *o = NULL; return E_NOINTERFACE; }
    IBaseFilter_AddRef(&p->f->bf);
    return S_OK;
}
static ULONG WINAPI AP_AddRef(IPin *t) { return IBaseFilter_AddRef(&A_PIN(t)->f->bf); }
static ULONG WINAPI AP_Release(IPin *t) { return IBaseFilter_Release(&A_PIN(t)->f->bf); }
static HRESULT WINAPI AP_Connect(IPin *t, IPin *recv, const AM_MEDIA_TYPE *m)
{
    APin *p = A_PIN(t); AM_MEDIA_TYPE use; HRESULT hr;
    logmsg("AudioEnc %s pin: Connect", p->out ? "out" : "in");
    if (!p->out) return E_UNEXPECTED;
    if (p->peer) return E_ALREADY_CONN_;
    if (p->f->state != State_Stopped) return E_NOT_STOPPED_;
    if (!p->f->in.peer) return E_NOT_CONNECTED_;
    memset(&use, 0, sizeof use);
    mt_copy(&use, (m && pcm_ok(m)) ? m : &p->f->in.mt);
    hr = IPin_ReceiveConnection(recv, &p->pin, &use);
    logmsg("   downstream ReceiveConnection -> %lx", hr);
    if (FAILED(hr)) { mt_free(&use); return hr; }
    mt_free(&p->mt); p->mt = use;
    IPin_AddRef(recv); p->peer = recv;
    if (SUCCEEDED(IPin_QueryInterface(recv, &IID_IMemInputPin, (void **)&p->peer_mip))) {
        IMemAllocator *a = p->f->in.alloc;
        if (a) IMemAllocator_AddRef(a); else IMemInputPin_GetAllocator(p->peer_mip, &a);
        if (a) { IMemInputPin_NotifyAllocator(p->peer_mip, a, FALSE); IMemAllocator_Release(a); }
    }
    return S_OK;
}
static HRESULT WINAPI AP_ReceiveConnection(IPin *t, IPin *conn, const AM_MEDIA_TYPE *m)
{
    APin *p = A_PIN(t);
    logmsg("AudioEnc in pin: ReceiveConnection major=%s sub=%s", m ? guidstr(&m->majortype) : "-", m ? guidstr(&m->subtype) : "-");
    if (p->out) return E_UNEXPECTED;
    if (p->peer) return E_ALREADY_CONN_;
    if (p->f->state != State_Stopped) return E_NOT_STOPPED_;
    if (!pcm_ok(m)) return E_NOT_ACCEPTED_;
    mt_free(&p->mt); mt_copy(&p->mt, m);
    IPin_AddRef(conn); p->peer = conn;
    return S_OK;
}
static HRESULT WINAPI AP_Disconnect(IPin *t)
{
    APin *p = A_PIN(t);
    if (p->f->state != State_Stopped) return E_NOT_STOPPED_;
    if (!p->peer) return S_FALSE;
    if (p->peer_mip) { IMemInputPin_Release(p->peer_mip); p->peer_mip = NULL; }
    if (p->alloc) { IMemAllocator_Release(p->alloc); p->alloc = NULL; }
    IPin_Release(p->peer); p->peer = NULL; mt_free(&p->mt);
    return S_OK;
}
static HRESULT WINAPI AP_ConnectedTo(IPin *t, IPin **o)
{
    APin *p = A_PIN(t);
    if (!p->peer) { *o = NULL; return E_NOT_CONNECTED_; }
    IPin_AddRef(p->peer); *o = p->peer; return S_OK;
}
static HRESULT WINAPI AP_ConnectionMediaType(IPin *t, AM_MEDIA_TYPE *m)
{
    APin *p = A_PIN(t);
    memset(m, 0, sizeof *m);
    if (!p->peer) return E_NOT_CONNECTED_;
    mt_copy(m, &p->mt); return S_OK;
}
static HRESULT WINAPI AP_QueryPinInfo(IPin *t, PIN_INFO *i)
{
    APin *p = A_PIN(t);
    i->pFilter = &p->f->bf; IBaseFilter_AddRef(i->pFilter);
    i->dir = p->out ? PINDIR_OUTPUT : PINDIR_INPUT; lstrcpyW(i->achName, p->out ? L"Out" : L"In"); return S_OK;
}
static HRESULT WINAPI AP_QueryDirection(IPin *t, PIN_DIRECTION *d) { *d = A_PIN(t)->out ? PINDIR_OUTPUT : PINDIR_INPUT; return S_OK; }
static HRESULT WINAPI AP_QueryId(IPin *t, LPWSTR *id) { *id = CoTaskMemAlloc(8); lstrcpyW(*id, A_PIN(t)->out ? L"Out" : L"In"); return S_OK; }
static HRESULT WINAPI AP_QueryAccept(IPin *t, const AM_MEDIA_TYPE *m) { return pcm_ok(m) ? S_OK : S_FALSE; }
typedef struct { IEnumMediaTypes e; LONG ref; } AEM;
static HRESULT WINAPI AEM_QI(IEnumMediaTypes *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IEnumMediaTypes)) { *o = t; ((AEM *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI AEM_AddRef(IEnumMediaTypes *t) { return ++((AEM *)t)->ref; }
static ULONG WINAPI AEM_Release(IEnumMediaTypes *t) { AEM *e = (AEM *)t; if (!--e->ref) { free(e); return 0; } return e->ref; }
static HRESULT WINAPI AEM_Next(IEnumMediaTypes *t, ULONG n, AM_MEDIA_TYPE **m, ULONG *g) { if (g) *g = 0; return S_FALSE; }
static HRESULT WINAPI AEM_Skip(IEnumMediaTypes *t, ULONG n) { return S_FALSE; }
static HRESULT WINAPI AEM_Reset(IEnumMediaTypes *t) { return S_OK; }
static HRESULT WINAPI AEM_Clone(IEnumMediaTypes *t, IEnumMediaTypes **o) { AEM *e = calloc(1, sizeof *e); e->e.lpVtbl = t->lpVtbl; e->ref = 1; *o = &e->e; return S_OK; }
static const IEnumMediaTypesVtbl aem_vtbl = {AEM_QI, AEM_AddRef, AEM_Release, AEM_Next, AEM_Skip, AEM_Reset, AEM_Clone};
static HRESULT WINAPI AP_EnumMediaTypes(IPin *t, IEnumMediaTypes **o) { AEM *e = calloc(1, sizeof *e); e->e.lpVtbl = &aem_vtbl; e->ref = 1; *o = &e->e; return S_OK; }
static HRESULT WINAPI AP_QueryInternalConnections(IPin *t, IPin **p, ULONG *n) { return E_NOTIMPL; }
static HRESULT WINAPI AP_EndOfStream(IPin *t)
{
    APin *p = A_PIN(t);
    logmsg("AudioEnc: EndOfStream");
    if (!p->out && p->f->out.peer) return IPin_EndOfStream(p->f->out.peer);
    return S_OK;
}
static HRESULT WINAPI AP_BeginFlush(IPin *t) { APin *p = A_PIN(t); return (!p->out && p->f->out.peer) ? IPin_BeginFlush(p->f->out.peer) : S_OK; }
static HRESULT WINAPI AP_EndFlush(IPin *t) { APin *p = A_PIN(t); return (!p->out && p->f->out.peer) ? IPin_EndFlush(p->f->out.peer) : S_OK; }
static HRESULT WINAPI AP_NewSegment(IPin *t, REFERENCE_TIME a, REFERENCE_TIME b, double r) { APin *p = A_PIN(t); return (!p->out && p->f->out.peer) ? IPin_NewSegment(p->f->out.peer, a, b, r) : S_OK; }
static const IPinVtbl ap_vtbl = {AP_QI, AP_AddRef, AP_Release, AP_Connect, AP_ReceiveConnection, AP_Disconnect, AP_ConnectedTo,
    AP_ConnectionMediaType, AP_QueryPinInfo, AP_QueryDirection, AP_QueryId, AP_QueryAccept, AP_EnumMediaTypes,
    AP_QueryInternalConnections, AP_EndOfStream, AP_BeginFlush, AP_EndFlush, AP_NewSegment};

static HRESULT WINAPI AM_QI(IMemInputPin *t, REFIID r, void **o) { return AP_QI(&A_MIP(t)->pin, r, o); }
static ULONG WINAPI AM_AddRef(IMemInputPin *t) { return IBaseFilter_AddRef(&A_MIP(t)->f->bf); }
static ULONG WINAPI AM_Release(IMemInputPin *t) { return IBaseFilter_Release(&A_MIP(t)->f->bf); }
static HRESULT WINAPI AM_GetAllocator(IMemInputPin *t, IMemAllocator **o) { return CoCreateInstance(&G_MemAlloc, NULL, CLSCTX_INPROC_SERVER, &IID_IMemAllocator, (void **)o); }
static HRESULT WINAPI AM_NotifyAllocator(IMemInputPin *t, IMemAllocator *a, BOOL ro)
{
    APin *p = A_MIP(t);
    if (p->alloc) IMemAllocator_Release(p->alloc);
    p->alloc = a; if (a) IMemAllocator_AddRef(a);
    return S_OK;
}
static HRESULT WINAPI AM_GetAllocatorRequirements(IMemInputPin *t, ALLOCATOR_PROPERTIES *p) { return E_NOTIMPL; }
static HRESULT WINAPI AM_Receive(IMemInputPin *t, IMediaSample *ms)
{
    APin *p = A_MIP(t); IMemInputPin *d = p->f->out.peer_mip;
    if (!d) return E_NOT_CONNECTED_;
    return IMemInputPin_Receive(d, ms); /* PCM in, PCM out: hand the very same sample to the writer */
}
static HRESULT WINAPI AM_ReceiveMultiple(IMemInputPin *t, IMediaSample **s, LONG n, LONG *done)
{
    LONG i; HRESULT hr = S_OK;
    for (i = 0; i < n && hr == S_OK; i++) hr = AM_Receive(t, s[i]);
    *done = i; return hr;
}
static HRESULT WINAPI AM_ReceiveCanBlock(IMemInputPin *t) { return S_OK; }
static const IMemInputPinVtbl am_vtbl = {AM_QI, AM_AddRef, AM_Release, AM_GetAllocator, AM_NotifyAllocator, AM_GetAllocatorRequirements,
    AM_Receive, AM_ReceiveMultiple, AM_ReceiveCanBlock};

typedef struct { IEnumPins e; LONG ref; AE *f; int idx; } AEP;
static HRESULT WINAPI AEP_QI(IEnumPins *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IEnumPins)) { *o = t; ((AEP *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI AEP_AddRef(IEnumPins *t) { return ++((AEP *)t)->ref; }
static ULONG WINAPI AEP_Release(IEnumPins *t) { AEP *e = (AEP *)t; if (!--e->ref) { IBaseFilter_Release(&e->f->bf); free(e); return 0; } return e->ref; }
static HRESULT WINAPI AEP_Next(IEnumPins *t, ULONG n, IPin **pins, ULONG *got)
{
    AEP *e = (AEP *)t; ULONG k = 0;
    while (k < n && e->idx < 2) { APin *p = e->idx ? &e->f->out : &e->f->in; IPin_AddRef(&p->pin); pins[k++] = &p->pin; e->idx++; }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI AEP_Skip(IEnumPins *t, ULONG n) { AEP *e = (AEP *)t; e->idx += n; return e->idx > 2 ? S_FALSE : S_OK; }
static HRESULT WINAPI AEP_Reset(IEnumPins *t) { ((AEP *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI AEP_Clone(IEnumPins *t, IEnumPins **o) { AEP *e = (AEP *)t, *n = calloc(1, sizeof *n); *n = *e; n->ref = 1; IBaseFilter_AddRef(&n->f->bf); *o = &n->e; return S_OK; }
static const IEnumPinsVtbl aep_vtbl = {AEP_QI, AEP_AddRef, AEP_Release, AEP_Next, AEP_Skip, AEP_Reset, AEP_Clone};

static HRESULT WINAPI AB_QI(IBaseFilter *t, REFIID r, void **o)
{
    AE *f = A_BF(t);
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IPersist) || IsEqualGUID(r, &IID_IMediaFilter) || IsEqualGUID(r, &IID_IBaseFilter)) *o = &f->bf;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&f->ref);
    return S_OK;
}
static ULONG WINAPI AB_AddRef(IBaseFilter *t) { return InterlockedIncrement(&A_BF(t)->ref); }
static ULONG WINAPI AB_Release(IBaseFilter *t)
{
    AE *f = A_BF(t);
    LONG n = InterlockedDecrement(&f->ref);
    if (!n) {
        logmsg("AudioEnc destroyed");
        if (f->in.peer) IPin_Release(f->in.peer);
        if (f->out.peer) IPin_Release(f->out.peer);
        if (f->out.peer_mip) IMemInputPin_Release(f->out.peer_mip);
        if (f->in.alloc) IMemAllocator_Release(f->in.alloc);
        mt_free(&f->in.mt); mt_free(&f->out.mt);
        free(f);
    }
    return n;
}
static HRESULT WINAPI AB_GetClassID(IBaseFilter *t, CLSID *c) { *c = CLSID_AudioEncPass; return S_OK; }
static HRESULT WINAPI AB_Stop(IBaseFilter *t) { A_BF(t)->state = State_Stopped; return S_OK; }
static HRESULT WINAPI AB_Pause(IBaseFilter *t) { A_BF(t)->state = State_Paused; return S_OK; }
static HRESULT WINAPI AB_Run(IBaseFilter *t, REFERENCE_TIME s) { A_BF(t)->state = State_Running; return S_OK; }
static HRESULT WINAPI AB_GetState(IBaseFilter *t, DWORD ms, FILTER_STATE *s) { *s = A_BF(t)->state; return S_OK; }
static HRESULT WINAPI AB_SetSyncSource(IBaseFilter *t, IReferenceClock *c) { return S_OK; }
static HRESULT WINAPI AB_GetSyncSource(IBaseFilter *t, IReferenceClock **c) { *c = NULL; return S_OK; }
static HRESULT WINAPI AB_EnumPins(IBaseFilter *t, IEnumPins **o)
{
    AEP *e = calloc(1, sizeof *e);
    e->e.lpVtbl = &aep_vtbl; e->ref = 1; e->f = A_BF(t); IBaseFilter_AddRef(t);
    *o = &e->e; return S_OK;
}
static HRESULT WINAPI AB_FindPin(IBaseFilter *t, LPCWSTR id, IPin **o)
{
    AE *f = A_BF(t);
    if (!lstrcmpiW(id, L"In")) { IPin_AddRef(&f->in.pin); *o = &f->in.pin; return S_OK; }
    if (!lstrcmpiW(id, L"Out")) { IPin_AddRef(&f->out.pin); *o = &f->out.pin; return S_OK; }
    *o = NULL; return E_NOT_FOUND_;
}
static HRESULT WINAPI AB_QueryFilterInfo(IBaseFilter *t, FILTER_INFO *i)
{
    AE *f = A_BF(t);
    lstrcpyW(i->achName, f->gname);
    i->pGraph = f->graph; if (f->graph) IFilterGraph_AddRef(f->graph);
    return S_OK;
}
static HRESULT WINAPI AB_JoinFilterGraph(IBaseFilter *t, IFilterGraph *g, LPCWSTR n)
{
    AE *f = A_BF(t);
    f->graph = g;
    if (n) lstrcpynW(f->gname, n, 128); else f->gname[0] = 0;
    return S_OK;
}
static HRESULT WINAPI AB_QueryVendorInfo(IBaseFilter *t, LPWSTR *v) { return E_NOTIMPL; }
static const IBaseFilterVtbl ab_vtbl = {AB_QI, AB_AddRef, AB_Release, AB_GetClassID, AB_Stop, AB_Pause, AB_Run, AB_GetState,
    AB_SetSyncSource, AB_GetSyncSource, AB_EnumPins, AB_FindPin, AB_QueryFilterInfo, AB_JoinFilterGraph, AB_QueryVendorInfo};

static HRESULT audioenc_create(REFIID riid, void **o)
{
    AE *f = calloc(1, sizeof *f); HRESULT hr;
    f->bf.lpVtbl = &ab_vtbl; f->ref = 1; f->state = State_Stopped;
    f->in.pin.lpVtbl = &ap_vtbl; f->in.mip.lpVtbl = &am_vtbl; f->in.f = f; f->in.out = 0;
    f->out.pin.lpVtbl = &ap_vtbl; f->out.f = f; f->out.out = 1;
    hr = AB_QI(&f->bf, riid, o);
    AB_Release(&f->bf);
    return hr;
}

/* =========================== property bag / moniker / enumerator =========================== */
typedef struct { IPropertyBag pb; LONG ref; } PB;
static HRESULT WINAPI PB_QI(IPropertyBag *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IPropertyBag)) { *o = t; ((PB *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI PB_AddRef(IPropertyBag *t) { return ++((PB *)t)->ref; }
static ULONG WINAPI PB_Release(IPropertyBag *t) { PB *p = (PB *)t; if (!--p->ref) { free(p); return 0; } return p->ref; }
static HRESULT WINAPI PB_Read(IPropertyBag *t, LPCOLESTR name, VARIANT *v, IErrorLog *el)
{
    WCHAR buf[64];
    logmsg("AudioEnc PropertyBag Read(%ls)", name);
    if (!lstrcmpiW(name, L"FriendlyName")) { V_VT(v) = VT_BSTR; V_BSTR(v) = SysAllocString(FAUDIO_NAME); return S_OK; }
    if (!lstrcmpiW(name, L"CLSID")) {
        StringFromGUID2(&CLSID_AudioEncPass, buf, 64);
        V_VT(v) = VT_BSTR; V_BSTR(v) = SysAllocString(buf); return S_OK;
    }
    return E_FAIL;
}
static HRESULT WINAPI PB_Write(IPropertyBag *t, LPCOLESTR name, VARIANT *v) { return E_NOTIMPL; }
static const IPropertyBagVtbl pb_vtbl = {PB_QI, PB_AddRef, PB_Release, PB_Read, PB_Write};

typedef struct { IMoniker m; LONG ref; } MK;
static HRESULT WINAPI MK_QI(IMoniker *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IMoniker) || IsEqualGUID(r, &IID_IPersistStream) || IsEqualGUID(r, &IID_IPersist)) { *o = t; ((MK *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI MK_AddRef(IMoniker *t) { return ++((MK *)t)->ref; }
static ULONG WINAPI MK_Release(IMoniker *t) { MK *m = (MK *)t; if (!--m->ref) { free(m); return 0; } return m->ref; }
static HRESULT WINAPI MK_GetClassID(IMoniker *t, CLSID *c) { *c = CLSID_NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_IsDirty(IMoniker *t) { return S_FALSE; }
static HRESULT WINAPI MK_Load(IMoniker *t, IStream *s) { return E_NOTIMPL; }
static HRESULT WINAPI MK_Save(IMoniker *t, IStream *s, BOOL c) { return E_NOTIMPL; }
static HRESULT WINAPI MK_GetSizeMax(IMoniker *t, ULARGE_INTEGER *n) { return E_NOTIMPL; }
static HRESULT WINAPI MK_BindToObject(IMoniker *t, IBindCtx *c, IMoniker *l, REFIID r, void **o)
{
    logmsg("AudioEnc moniker BindToObject(%s)", guidstr(r));
    return audioenc_create(r, o);
}
static HRESULT WINAPI MK_BindToStorage(IMoniker *t, IBindCtx *c, IMoniker *l, REFIID r, void **o)
{
    PB *p;
    logmsg("AudioEnc moniker BindToStorage(%s)", guidstr(r));
    if (!IsEqualGUID(r, &IID_IPropertyBag)) { *o = NULL; return E_NOINTERFACE; }
    p = calloc(1, sizeof *p); p->pb.lpVtbl = &pb_vtbl; p->ref = 1; *o = &p->pb; return S_OK;
}
static HRESULT WINAPI MK_Reduce(IMoniker *t, IBindCtx *c, DWORD d, IMoniker **l, IMoniker **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_ComposeWith(IMoniker *t, IMoniker *r, BOOL i, IMoniker **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_Enum(IMoniker *t, BOOL f, IEnumMoniker **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_IsEqual(IMoniker *t, IMoniker *o) { return o == t ? S_OK : S_FALSE; }
static HRESULT WINAPI MK_Hash(IMoniker *t, DWORD *h) { *h = 0x4144504d; return S_OK; }
static HRESULT WINAPI MK_IsRunning(IMoniker *t, IBindCtx *c, IMoniker *l, IMoniker *n) { return S_FALSE; }
static HRESULT WINAPI MK_GetTimeOfLastChange(IMoniker *t, IBindCtx *c, IMoniker *l, FILETIME *f) { return E_NOTIMPL; }
static HRESULT WINAPI MK_Inverse(IMoniker *t, IMoniker **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_CommonPrefixWith(IMoniker *t, IMoniker *o, IMoniker **p) { *p = NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_RelativePathTo(IMoniker *t, IMoniker *o, IMoniker **p) { *p = NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_GetDisplayName(IMoniker *t, IBindCtx *c, IMoniker *l, LPOLESTR *n)
{
    *n = CoTaskMemAlloc(64); lstrcpyW(*n, L"@device:cm:{33D9A761-90C8-11D0-BD43-00A0C911CE86}\\Microsoft ADPCM"); return S_OK;
}
static HRESULT WINAPI MK_ParseDisplayName(IMoniker *t, IBindCtx *c, IMoniker *l, LPOLESTR n, ULONG *e, IMoniker **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT WINAPI MK_IsSystemMoniker(IMoniker *t, DWORD *d) { *d = 0; return S_FALSE; }
static const IMonikerVtbl mk_vtbl = {MK_QI, MK_AddRef, MK_Release, MK_GetClassID, MK_IsDirty, MK_Load, MK_Save, MK_GetSizeMax,
    MK_BindToObject, MK_BindToStorage, MK_Reduce, MK_ComposeWith, MK_Enum, MK_IsEqual, MK_Hash, MK_IsRunning,
    MK_GetTimeOfLastChange, MK_Inverse, MK_CommonPrefixWith, MK_RelativePathTo, MK_GetDisplayName, MK_ParseDisplayName, MK_IsSystemMoniker};

typedef struct { IEnumMoniker e; LONG ref; int idx; } EM;
static HRESULT WINAPI EM_QI(IEnumMoniker *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IEnumMoniker)) { *o = t; ((EM *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI EM_AddRef(IEnumMoniker *t) { return ++((EM *)t)->ref; }
static ULONG WINAPI EM_Release(IEnumMoniker *t) { EM *e = (EM *)t; if (!--e->ref) { free(e); return 0; } return e->ref; }
static HRESULT WINAPI EM_Next(IEnumMoniker *t, ULONG n, IMoniker **m, ULONG *got)
{
    EM *e = (EM *)t; ULONG k = 0;
    while (k < n && e->idx < 1) { MK *mk = calloc(1, sizeof *mk); mk->m.lpVtbl = &mk_vtbl; mk->ref = 1; m[k++] = &mk->m; e->idx++; }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI EM_Skip(IEnumMoniker *t, ULONG n) { EM *e = (EM *)t; e->idx += n; return e->idx > 1 ? S_FALSE : S_OK; }
static HRESULT WINAPI EM_Reset(IEnumMoniker *t) { ((EM *)t)->idx = 0; return S_OK; }
static HRESULT WINAPI EM_Clone(IEnumMoniker *t, IEnumMoniker **o) { EM *e = (EM *)t, *n = calloc(1, sizeof *n); *n = *e; n->ref = 1; *o = &n->e; return S_OK; }
static const IEnumMonikerVtbl em_vtbl = {EM_QI, EM_AddRef, EM_Release, EM_Next, EM_Skip, EM_Reset, EM_Clone};

/* =========================== ICreateDevEnum =========================== */
typedef struct { ICreateDevEnum de; LONG ref; ICreateDevEnum *real; } DE;
static ICreateDevEnum *real_devenum(void)
{
    typedef HRESULT (WINAPI *GCO)(REFCLSID, REFIID, void **);
    WCHAR path[MAX_PATH]; HMODULE m; GCO g; IClassFactory *cf = NULL; ICreateDevEnum *r = NULL;
    GetSystemDirectoryW(path, MAX_PATH); lstrcatW(path, L"\\devenum.dll");
    m = LoadLibraryW(path);
    if (!m || !(g = (GCO)GetProcAddress(m, "DllGetClassObject"))) { logmsg("devenum: cannot load builtin devenum.dll"); return NULL; }
    if (SUCCEEDED(g(&CLSID_SysDevEnum_, &IID_IClassFactory, (void **)&cf)) && cf) {
        IClassFactory_CreateInstance(cf, NULL, &IID_ICreateDevEnum, (void **)&r);
        IClassFactory_Release(cf);
    }
    return r;
}
static HRESULT WINAPI DE_QI(ICreateDevEnum *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_ICreateDevEnum)) { *o = t; ((DE *)t)->ref++; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI DE_AddRef(ICreateDevEnum *t) { return ++((DE *)t)->ref; }
static ULONG WINAPI DE_Release(ICreateDevEnum *t) { DE *d = (DE *)t; if (!--d->ref) { if (d->real) ICreateDevEnum_Release(d->real); free(d); return 0; } return d->ref; }
static HRESULT WINAPI DE_CreateClassEnumerator(ICreateDevEnum *t, REFCLSID cls, IEnumMoniker **o, DWORD fl)
{
    DE *d = (DE *)t;
    logmsg("DevEnum CreateClassEnumerator(%s)", guidstr(cls));
    if (IsEqualGUID(cls, &CAT_AudioCompressor)) {
        EM *e = calloc(1, sizeof *e); e->e.lpVtbl = &em_vtbl; e->ref = 1; *o = &e->e; return S_OK;
    }
    if (!d->real) d->real = real_devenum();
    if (!d->real) { *o = NULL; return S_FALSE; }
    return ICreateDevEnum_CreateClassEnumerator(d->real, cls, o, fl);
}
static const ICreateDevEnumVtbl de_vtbl = {DE_QI, DE_AddRef, DE_Release, DE_CreateClassEnumerator};

typedef struct { IClassFactory cf; } DCF;
static HRESULT WINAPI DCF_QI(IClassFactory *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_Unk) || IsEqualGUID(r, &IID_IClassFactory)) { *o = t; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI DCF_AddRef(IClassFactory *t) { return 2; }
static ULONG WINAPI DCF_Release(IClassFactory *t) { return 1; }
static HRESULT WINAPI DCF_Create(IClassFactory *t, IUnknown *outer, REFIID r, void **o)
{
    DE *d; HRESULT hr;
    logmsg("DevEnum CreateInstance(%s)", guidstr(r));
    if (outer) return CLASS_E_NOAGGREGATION;
    d = calloc(1, sizeof *d); d->de.lpVtbl = &de_vtbl; d->ref = 1;
    hr = DE_QI(&d->de, r, o);
    DE_Release(&d->de);
    return hr;
}
static HRESULT WINAPI DCF_Lock(IClassFactory *t, BOOL l) { return S_OK; }
static const IClassFactoryVtbl dcf_vtbl = {DCF_QI, DCF_AddRef, DCF_Release, DCF_Create, DCF_Lock};
static DCF the_dcf = {{&dcf_vtbl}};

int devenum_clsid(REFCLSID c) { return IsEqualGUID(c, &CLSID_SysDevEnum_); }
HRESULT devenum_get_factory(REFIID r, void **o) { return DCF_QI(&the_dcf.cf, r, o); }
