/* SPDX-License-Identifier: GPL-3.0-or-later */
/* IWMProfileManager / IWMProfile / IWMStreamConfig implementation that can parse Windows Media profile XML (.prx). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WMCreateSyncReader hdr_WMCreateSyncReader
#define WMCreateProfileManager hdr_WMCreateProfileManager
#include <windows.h>
#include <ole2.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <wmsdk.h>
#include <nserror.h>
#undef WMCreateSyncReader
#undef WMCreateProfileManager

void logmsg(const char *fmt, ...);
const char *guidstr(const GUID *g);
HRESULT profile_manager_create(IWMProfileManager **pp);

static const GUID G_IUnknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID G_IWMProfileManager = {0xd16679f2, 0x6ca0, 0x472d, {0x8d, 0x31, 0x2f, 0x5d, 0x55, 0xae, 0xe1, 0x55}};
static const GUID G_IWMProfile = {0x96406bdb, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_IWMStreamConfig = {0x96406bdc, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_IWMMediaProps = {0x96406bce, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_IWMVideoMediaProps = {0x96406bcf, 0x2b2b, 0x11d3, {0xb3, 0x6b, 0x00, 0xc0, 0x4f, 0x61, 0x08, 0xff}};
static const GUID G_Video = {0x73646976, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_Audio = {0x73647561, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static const GUID G_FmtVideo = {0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID G_FmtWave = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};

/* custom IWMVideoMediaProps vtable (not in mingw headers) */
typedef struct IWMVideoMediaPropsX IWMVideoMediaPropsX;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(IWMVideoMediaPropsX *, REFIID, void **);
    ULONG (WINAPI *AddRef)(IWMVideoMediaPropsX *);
    ULONG (WINAPI *Release)(IWMVideoMediaPropsX *);
    HRESULT (WINAPI *GetType)(IWMVideoMediaPropsX *, GUID *);
    HRESULT (WINAPI *GetMediaType)(IWMVideoMediaPropsX *, WM_MEDIA_TYPE *, DWORD *);
    HRESULT (WINAPI *SetMediaType)(IWMVideoMediaPropsX *, WM_MEDIA_TYPE *);
    HRESULT (WINAPI *GetMaxKeyFrameSpacing)(IWMVideoMediaPropsX *, LONGLONG *);
    HRESULT (WINAPI *SetMaxKeyFrameSpacing)(IWMVideoMediaPropsX *, LONGLONG);
    HRESULT (WINAPI *GetQuality)(IWMVideoMediaPropsX *, DWORD *);
    HRESULT (WINAPI *SetQuality)(IWMVideoMediaPropsX *, DWORD);
} IWMVideoMediaPropsXVtbl;
struct IWMVideoMediaPropsX { const IWMVideoMediaPropsXVtbl *lpVtbl; };

#pragma pack(push, 1)
typedef struct { LONG left, top, right, bottom; } RC;
typedef struct { RC rcSource, rcTarget; DWORD dwBitRate, dwBitErrorRate; LONGLONG AvgTimePerFrame; BITMAPINFOHEADER bmi; } VIH;
#pragma pack(pop)

/* ---------- stream config object ---------- */
typedef struct {
    IWMStreamConfig sc;
    IWMMediaProps mp;
    IWMVideoMediaPropsX vp;
    LONG ref;
    GUID type;
    WORD num;
    WCHAR name[64], conn[64];
    DWORD bitrate, bufwin;
    LONGLONG keyspace; DWORD quality;
    WM_MEDIA_TYPE mt; BYTE fmt[1024];
} PSC;
#define SC_SC(p) ((PSC *)(p))
#define SC_MP(p) ((PSC *)((BYTE *)(p) - offsetof(PSC, mp)))
#define SC_VP(p) ((PSC *)((BYTE *)(p) - offsetof(PSC, vp)))

static HRESULT WINAPI SC_QI(IWMStreamConfig *t, REFIID r, void **o)
{
    PSC *s = SC_SC(t);
    if (IsEqualGUID(r, &G_IUnknown) || IsEqualGUID(r, &G_IWMStreamConfig)) *o = &s->sc;
    else if (IsEqualGUID(r, &G_IWMMediaProps)) *o = &s->mp;
    else if (IsEqualGUID(r, &G_IWMVideoMediaProps) && IsEqualGUID(&s->type, &G_Video)) *o = &s->vp;
    else { logmsg("StreamConfig QI %s -> E_NOINTERFACE", guidstr(r)); *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG WINAPI SC_AddRef(IWMStreamConfig *t) { return InterlockedIncrement(&SC_SC(t)->ref); }
static ULONG WINAPI SC_Release(IWMStreamConfig *t)
{
    LONG n = InterlockedDecrement(&SC_SC(t)->ref);
    if (!n) free(t);
    return n;
}
static HRESULT WINAPI SC_GetStreamType(IWMStreamConfig *t, GUID *g) { *g = SC_SC(t)->type; return S_OK; }
static HRESULT WINAPI SC_GetStreamNumber(IWMStreamConfig *t, WORD *n) { *n = SC_SC(t)->num; return S_OK; }
static HRESULT WINAPI SC_SetStreamNumber(IWMStreamConfig *t, WORD n) { SC_SC(t)->num = n; return S_OK; }
static HRESULT getstr(const WCHAR *src, WCHAR *dst, WORD *len)
{
    WORD need = (WORD)(lstrlenW(src) + 1);
    if (!len) return E_INVALIDARG;
    if (!dst) { *len = need; return S_OK; }
    if (*len < need) { *len = need; return NS_E_SDK_BUFFERTOOSMALL; }
    lstrcpyW(dst, src); *len = need; return S_OK;
}
static HRESULT WINAPI SC_GetStreamName(IWMStreamConfig *t, WCHAR *n, WORD *l) { return getstr(SC_SC(t)->name, n, l); }
static HRESULT WINAPI SC_SetStreamName(IWMStreamConfig *t, const WCHAR *n) { lstrcpynW(SC_SC(t)->name, n, 64); return S_OK; }
static HRESULT WINAPI SC_GetConnectionName(IWMStreamConfig *t, WCHAR *n, WORD *l) { return getstr(SC_SC(t)->conn, n, l); }
static HRESULT WINAPI SC_SetConnectionName(IWMStreamConfig *t, const WCHAR *n) { lstrcpynW(SC_SC(t)->conn, n, 64); return S_OK; }
static HRESULT WINAPI SC_GetBitrate(IWMStreamConfig *t, DWORD *b) { *b = SC_SC(t)->bitrate; return S_OK; }
static HRESULT WINAPI SC_SetBitrate(IWMStreamConfig *t, DWORD b) { logmsg("StreamConfig SetBitrate(%lu)", b); SC_SC(t)->bitrate = b; return S_OK; }
static HRESULT WINAPI SC_GetBufferWindow(IWMStreamConfig *t, DWORD *b) { *b = SC_SC(t)->bufwin; return S_OK; }
static HRESULT WINAPI SC_SetBufferWindow(IWMStreamConfig *t, DWORD b) { SC_SC(t)->bufwin = b; return S_OK; }
static const IWMStreamConfigVtbl sc_vtbl = {SC_QI, SC_AddRef, SC_Release, SC_GetStreamType, SC_GetStreamNumber, SC_SetStreamNumber,
    SC_GetStreamName, SC_SetStreamName, SC_GetConnectionName, SC_SetConnectionName, SC_GetBitrate, SC_SetBitrate, SC_GetBufferWindow, SC_SetBufferWindow};

/* IWMMediaProps */
static HRESULT WINAPI MP_QI(IWMMediaProps *t, REFIID r, void **o) { return SC_QI(&SC_MP(t)->sc, r, o); }
static ULONG WINAPI MP_AddRef(IWMMediaProps *t) { return SC_AddRef(&SC_MP(t)->sc); }
static ULONG WINAPI MP_Release(IWMMediaProps *t) { return SC_Release(&SC_MP(t)->sc); }
static HRESULT WINAPI MP_GetType(IWMMediaProps *t, GUID *g) { *g = SC_MP(t)->mt.majortype; return S_OK; }
static HRESULT WINAPI MP_GetMediaType(IWMMediaProps *t, WM_MEDIA_TYPE *pt, DWORD *pcb)
{
    PSC *s = SC_MP(t);
    DWORD need = sizeof(WM_MEDIA_TYPE) + s->mt.cbFormat;
    if (!pcb) return E_INVALIDARG;
    if (!pt) { *pcb = need; return S_OK; }
    if (*pcb < need) { *pcb = need; return NS_E_SDK_BUFFERTOOSMALL; }
    *pt = s->mt; pt->pUnk = NULL; pt->pbFormat = (BYTE *)(pt + 1);
    memcpy(pt->pbFormat, s->fmt, s->mt.cbFormat);
    *pcb = need; return S_OK;
}
static HRESULT WINAPI MP_SetMediaType(IWMMediaProps *t, WM_MEDIA_TYPE *pt)
{
    PSC *s = SC_MP(t);
    if (!pt || pt->cbFormat > sizeof s->fmt) return E_INVALIDARG;
    logmsg("StreamConfig SetMediaType major=%s sub=%s cb=%lu", guidstr(&pt->majortype), guidstr(&pt->subtype), pt->cbFormat);
    s->mt = *pt; s->mt.pUnk = NULL;
    memcpy(s->fmt, pt->pbFormat, pt->cbFormat);
    s->mt.pbFormat = s->fmt; return S_OK;
}
static const IWMMediaPropsVtbl mp_vtbl = {MP_QI, MP_AddRef, MP_Release, MP_GetType, MP_GetMediaType, MP_SetMediaType};

/* IWMVideoMediaProps */
static HRESULT WINAPI VP_QI(IWMVideoMediaPropsX *t, REFIID r, void **o) { return SC_QI(&SC_VP(t)->sc, r, o); }
static ULONG WINAPI VP_AddRef(IWMVideoMediaPropsX *t) { return SC_AddRef(&SC_VP(t)->sc); }
static ULONG WINAPI VP_Release(IWMVideoMediaPropsX *t) { return SC_Release(&SC_VP(t)->sc); }
static HRESULT WINAPI VP_GetType(IWMVideoMediaPropsX *t, GUID *g) { *g = SC_VP(t)->mt.majortype; return S_OK; }
static HRESULT WINAPI VP_GetMediaType(IWMVideoMediaPropsX *t, WM_MEDIA_TYPE *pt, DWORD *pcb) { return MP_GetMediaType(&SC_VP(t)->mp, pt, pcb); }
static HRESULT WINAPI VP_SetMediaType(IWMVideoMediaPropsX *t, WM_MEDIA_TYPE *pt) { return MP_SetMediaType(&SC_VP(t)->mp, pt); }
static HRESULT WINAPI VP_GetMax(IWMVideoMediaPropsX *t, LONGLONG *v) { *v = SC_VP(t)->keyspace; return S_OK; }
static HRESULT WINAPI VP_SetMax(IWMVideoMediaPropsX *t, LONGLONG v) { SC_VP(t)->keyspace = v; return S_OK; }
static HRESULT WINAPI VP_GetQ(IWMVideoMediaPropsX *t, DWORD *v) { *v = SC_VP(t)->quality; return S_OK; }
static HRESULT WINAPI VP_SetQ(IWMVideoMediaPropsX *t, DWORD v) { logmsg("VideoMediaProps SetQuality(%lu)", v); SC_VP(t)->quality = v; return S_OK; }
static const IWMVideoMediaPropsXVtbl vp_vtbl = {VP_QI, VP_AddRef, VP_Release, VP_GetType, VP_GetMediaType, VP_SetMediaType, VP_GetMax, VP_SetMax, VP_GetQ, VP_SetQ};

static PSC *new_sc(const GUID *type)
{
    PSC *s = calloc(1, sizeof *s);
    s->sc.lpVtbl = &sc_vtbl; s->mp.lpVtbl = &mp_vtbl; s->vp.lpVtbl = &vp_vtbl; s->ref = 1;
    s->type = *type; s->num = 0; s->quality = 75; s->keyspace = 80000000;
    s->mt.majortype = *type;
    s->mt.pbFormat = s->fmt;
    return s;
}

/* ---------- XML-ish parsing ---------- */
typedef struct { char name[48]; char val[1100]; } Attr;
typedef struct { char tag[48]; Attr a[24]; int na; int closing; } Tag;

static int parse_tag(const char **pp, Tag *t)
{
    const char *p = *pp;
    memset(t, 0, sizeof *t);
    while (*p && *p != '<') p++;
    if (!*p) return 0;
    p++;
    if (*p == '?' || *p == '!') { while (*p && *p != '>') p++; if (*p) p++; *pp = p; t->tag[0] = 0; return 1; }
    if (*p == '/') { t->closing = 1; p++; }
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/') { if (i < 47) t->tag[i++] = (char)tolower((unsigned char)*p); p++; }
    while (*p && *p != '>') {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '/' || *p == '>') { if (*p == '/') p++; continue; }
        if (!*p) break;
        Attr *a = &t->a[t->na < 23 ? t->na : 23];
        i = 0;
        while (*p && *p != '=' && !isspace((unsigned char)*p) && *p != '>') { if (i < 47) a->name[i++] = (char)tolower((unsigned char)*p); p++; }
        a->name[i] = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=') {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            char q = *p;
            if (q == '"' || q == '\'') { p++; i = 0; while (*p && *p != q) { if (i < 1099) a->val[i++] = *p; p++; } a->val[i] = 0; if (*p) p++; }
        }
        if (t->na < 23) t->na++;
    }
    if (*p) p++;
    *pp = p;
    return 1;
}
static const char *attr(Tag *t, const char *n)
{
    for (int i = 0; i < t->na; i++) if (!strcmp(t->a[i].name, n)) return t->a[i].val;
    return NULL;
}
static long attr_l(Tag *t, const char *n, long def) { const char *v = attr(t, n); return v ? strtol(v, NULL, 0) : def; }
static int parse_guid(const char *s, GUID *g)
{
    unsigned d1, d2, d3, b[8];
    if (!s) return 0;
    while (*s == '{' || *s == ' ') s++;
    if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11) return 0;
    g->Data1 = d1; g->Data2 = (WORD)d2; g->Data3 = (WORD)d3;
    for (int i = 0; i < 8; i++) g->Data4[i] = (BYTE)b[i];
    return 1;
}
static DWORD parse_fourcc_or_num(const char *s)
{
    if (!s) return 0;
    int digits = 1;
    for (const char *p = s; *p; p++) if (!isdigit((unsigned char)*p)) digits = 0;
    if (digits || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) return (DWORD)strtoul(s, NULL, 0);
    if (strlen(s) == 4) return (DWORD)((BYTE)s[0] | (BYTE)s[1] << 8 | (BYTE)s[2] << 16 | (DWORD)(BYTE)s[3] << 24);
    return 0;
}
static int hexbytes(const char *s, BYTE *out, int max)
{
    int n = 0;
    while (s && s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (BYTE)v; s += 2;
    }
    return n;
}

/* ---------- profile object ---------- */
typedef struct {
    IWMProfile iface;
    LONG ref;
    PSC *streams[8]; int n;
    WCHAR name[64], desc[256];
} Prof;
#define PR(p) ((Prof *)(p))

static HRESULT WINAPI PF_QI(IWMProfile *t, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown) || IsEqualGUID(r, &G_IWMProfile)) { *o = t; InterlockedIncrement(&PR(t)->ref); return S_OK; }
    logmsg("Profile QI %s -> E_NOINTERFACE", guidstr(r));
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI PF_AddRef(IWMProfile *t) { return InterlockedIncrement(&PR(t)->ref); }
static ULONG WINAPI PF_Release(IWMProfile *t)
{
    Prof *p = PR(t);
    LONG n = InterlockedDecrement(&p->ref);
    if (!n) { for (int i = 0; i < p->n; i++) SC_Release(&p->streams[i]->sc); free(p); }
    return n;
}
static HRESULT WINAPI PF_GetVersion(IWMProfile *t, WMT_VERSION *v) { *v = WMT_VER_7_0; return S_OK; }
static HRESULT WINAPI PF_GetName(IWMProfile *t, WCHAR *n, DWORD *l)
{
    WORD w = (WORD)*l; HRESULT hr = getstr(PR(t)->name, n, &w); *l = w; return hr;
}
static HRESULT WINAPI PF_SetName(IWMProfile *t, const WCHAR *n) { lstrcpynW(PR(t)->name, n, 64); return S_OK; }
static HRESULT WINAPI PF_GetDescription(IWMProfile *t, WCHAR *n, DWORD *l)
{
    WORD w = (WORD)*l; HRESULT hr = getstr(PR(t)->desc, n, &w); *l = w; return hr;
}
static HRESULT WINAPI PF_SetDescription(IWMProfile *t, const WCHAR *n) { lstrcpynW(PR(t)->desc, n, 256); return S_OK; }
static HRESULT WINAPI PF_GetStreamCount(IWMProfile *t, DWORD *c) { *c = PR(t)->n; return S_OK; }
static HRESULT WINAPI PF_GetStream(IWMProfile *t, DWORD i, IWMStreamConfig **o)
{
    Prof *p = PR(t);
    if (i >= (DWORD)p->n) return E_INVALIDARG;
    SC_AddRef(&p->streams[i]->sc); *o = &p->streams[i]->sc; return S_OK;
}
static HRESULT WINAPI PF_GetStreamByNumber(IWMProfile *t, WORD n, IWMStreamConfig **o)
{
    Prof *p = PR(t);
    for (int i = 0; i < p->n; i++) if (p->streams[i]->num == n) { SC_AddRef(&p->streams[i]->sc); *o = &p->streams[i]->sc; return S_OK; }
    return NS_E_INVALID_REQUEST;
}
static HRESULT remove_at(Prof *p, int i)
{
    SC_Release(&p->streams[i]->sc);
    for (; i < p->n - 1; i++) p->streams[i] = p->streams[i + 1];
    p->n--; return S_OK;
}
static HRESULT WINAPI PF_RemoveStream(IWMProfile *t, IWMStreamConfig *c)
{
    Prof *p = PR(t);
    for (int i = 0; i < p->n; i++) if (&p->streams[i]->sc == c) return remove_at(p, i);
    return NS_E_INVALID_REQUEST;
}
static HRESULT WINAPI PF_RemoveStreamByNumber(IWMProfile *t, WORD n)
{
    Prof *p = PR(t);
    for (int i = 0; i < p->n; i++) if (p->streams[i]->num == n) return remove_at(p, i);
    return NS_E_INVALID_REQUEST;
}
static HRESULT WINAPI PF_AddStream(IWMProfile *t, IWMStreamConfig *c)
{
    Prof *p = PR(t);
    logmsg("Profile AddStream");
    if (p->n >= 8) return E_FAIL;
    /* the stream object may be ours; keep a reference */
    PSC *s = (PSC *)c;
    SC_AddRef(c);
    if (!s->num) s->num = (WORD)(p->n + 1);
    p->streams[p->n++] = s;
    return S_OK;
}
static HRESULT WINAPI PF_ReconfigStream(IWMProfile *t, IWMStreamConfig *c) { logmsg("Profile ReconfigStream"); return S_OK; }
static HRESULT WINAPI PF_CreateNewStream(IWMProfile *t, REFGUID type, IWMStreamConfig **o)
{
    logmsg("Profile CreateNewStream(%s)", guidstr(type));
    PSC *s = new_sc(type);
    *o = &s->sc; return S_OK;
}
static HRESULT WINAPI PF_GetMutualExclusionCount(IWMProfile *t, DWORD *c) { *c = 0; return S_OK; }
static HRESULT WINAPI PF_GetMutualExclusion(IWMProfile *t, DWORD i, IWMMutualExclusion **o) { return E_INVALIDARG; }
static HRESULT WINAPI PF_RemoveMutualExclusion(IWMProfile *t, IWMMutualExclusion *m) { return E_NOTIMPL; }
static HRESULT WINAPI PF_AddMutualExclusion(IWMProfile *t, IWMMutualExclusion *m) { return E_NOTIMPL; }
static HRESULT WINAPI PF_CreateNewMutualExclusion(IWMProfile *t, IWMMutualExclusion **m) { return E_NOTIMPL; }
static const IWMProfileVtbl pf_vtbl = {PF_QI, PF_AddRef, PF_Release, PF_GetVersion, PF_GetName, PF_SetName, PF_GetDescription,
    PF_SetDescription, PF_GetStreamCount, PF_GetStream, PF_GetStreamByNumber, PF_RemoveStream, PF_RemoveStreamByNumber,
    PF_AddStream, PF_ReconfigStream, PF_CreateNewStream, PF_GetMutualExclusionCount, PF_GetMutualExclusion,
    PF_RemoveMutualExclusion, PF_AddMutualExclusion, PF_CreateNewMutualExclusion};

static Prof *new_prof(void)
{
    Prof *p = calloc(1, sizeof *p);
    p->iface.lpVtbl = &pf_vtbl; p->ref = 1;
    return p;
}

static void finish_stream(PSC *s, const VIH *v, int have_v, const WAVEFORMATEX *w, int have_w, const BYTE *extra, int nextra)
{
    if (IsEqualGUID(&s->type, &G_Video) && have_v) {
        memcpy(s->fmt, v, sizeof *v);
        s->mt.formattype = G_FmtVideo; s->mt.cbFormat = sizeof *v;
    } else if (IsEqualGUID(&s->type, &G_Audio) && have_w) {
        memcpy(s->fmt, w, sizeof *w);
        if (nextra) memcpy(s->fmt + sizeof *w, extra, nextra);
        ((WAVEFORMATEX *)s->fmt)->cbSize = (WORD)nextra;
        s->mt.formattype = G_FmtWave; s->mt.cbFormat = sizeof *w + nextra;
    }
    s->mt.pbFormat = s->fmt;
}

static HRESULT parse_profile(const char *xml, Prof *p)
{
    const char *cur = xml;
    Tag t;
    PSC *s = NULL;
    VIH v; WAVEFORMATEX w; BYTE extra[256]; int nextra = 0, hv = 0, hw = 0;
    memset(&v, 0, sizeof v); memset(&w, 0, sizeof w);
    while (parse_tag(&cur, &t)) {
        if (!t.tag[0]) continue;
        if (!strcmp(t.tag, "profile") && !t.closing) {
            const char *n = attr(&t, "name"), *d = attr(&t, "description");
            if (n) MultiByteToWideChar(CP_UTF8, 0, n, -1, p->name, 64);
            if (d) MultiByteToWideChar(CP_UTF8, 0, d, -1, p->desc, 256);
        } else if (!strcmp(t.tag, "streamconfig")) {
            if (t.closing) {
                if (s) { finish_stream(s, &v, hv, &w, hw, extra, nextra); if (p->n < 8) p->streams[p->n++] = s; s = NULL; }
                continue;
            }
            GUID g = G_Video;
            parse_guid(attr(&t, "majortype"), &g);
            s = new_sc(&g);
            s->num = (WORD)attr_l(&t, "streamnumber", p->n + 1);
            s->bitrate = (DWORD)attr_l(&t, "bitrate", 0);
            s->bufwin = (DWORD)attr_l(&t, "bufferwindow", 0);
            const char *nm = attr(&t, "streamname"); if (nm) MultiByteToWideChar(CP_UTF8, 0, nm, -1, s->name, 64);
            nm = attr(&t, "inputname"); if (nm) MultiByteToWideChar(CP_UTF8, 0, nm, -1, s->conn, 64);
            memset(&v, 0, sizeof v); memset(&w, 0, sizeof w); nextra = 0; hv = hw = 0;
        } else if (s && !strcmp(t.tag, "videomediaprops")) {
            s->quality = (DWORD)attr_l(&t, "quality", 75);
            s->keyspace = (LONGLONG)strtoll(attr(&t, "maxkeyframespacing") ? attr(&t, "maxkeyframespacing") : "80000000", NULL, 0);
        } else if (s && !strcmp(t.tag, "wmmediatype")) {
            GUID sub; if (parse_guid(attr(&t, "subtype"), &sub)) s->mt.subtype = sub;
            s->mt.bFixedSizeSamples = (BOOL)attr_l(&t, "bfixedsizesamples", 0);
            s->mt.bTemporalCompression = (BOOL)attr_l(&t, "btemporalcompression", 0);
            s->mt.lSampleSize = (ULONG)attr_l(&t, "lsamplesize", 0);
        } else if (s && !strcmp(t.tag, "videoinfoheader")) {
            hv = 1;
            v.dwBitRate = (DWORD)attr_l(&t, "dwbitrate", 0);
            v.dwBitErrorRate = (DWORD)attr_l(&t, "dwbiterrorrate", 0);
            v.AvgTimePerFrame = attr(&t, "avgtimeperframe") ? strtoll(attr(&t, "avgtimeperframe"), NULL, 0) : 0;
        } else if (s && !strcmp(t.tag, "rcsource")) {
            v.rcSource = (RC){attr_l(&t, "left", 0), attr_l(&t, "top", 0), attr_l(&t, "right", 0), attr_l(&t, "bottom", 0)};
        } else if (s && !strcmp(t.tag, "rctarget")) {
            v.rcTarget = (RC){attr_l(&t, "left", 0), attr_l(&t, "top", 0), attr_l(&t, "right", 0), attr_l(&t, "bottom", 0)};
        } else if (s && !strcmp(t.tag, "bitmapinfoheader")) {
            v.bmi.biSize = sizeof(BITMAPINFOHEADER);
            v.bmi.biWidth = attr_l(&t, "biwidth", 0); v.bmi.biHeight = attr_l(&t, "biheight", 0);
            v.bmi.biPlanes = (WORD)attr_l(&t, "biplanes", 1); v.bmi.biBitCount = (WORD)attr_l(&t, "bibitcount", 24);
            v.bmi.biCompression = parse_fourcc_or_num(attr(&t, "bicompression"));
            v.bmi.biSizeImage = (DWORD)attr_l(&t, "bisizeimage", 0);
            v.bmi.biXPelsPerMeter = attr_l(&t, "bixpelspermeter", 0); v.bmi.biYPelsPerMeter = attr_l(&t, "biypelspermeter", 0);
        } else if (s && !strcmp(t.tag, "waveformatex")) {
            hw = 1;
            w.wFormatTag = (WORD)attr_l(&t, "wformattag", 1); w.nChannels = (WORD)attr_l(&t, "nchannels", 2);
            w.nSamplesPerSec = (DWORD)attr_l(&t, "nsamplespersec", 44100); w.nAvgBytesPerSec = (DWORD)attr_l(&t, "navgbytespersec", 0);
            w.nBlockAlign = (WORD)attr_l(&t, "nblockalign", 0); w.wBitsPerSample = (WORD)attr_l(&t, "wbitspersample", 16);
            nextra = hexbytes(attr(&t, "codecdata"), extra, sizeof extra);
        }
    }
    if (s) { finish_stream(s, &v, hv, &w, hw, extra, nextra); if (p->n < 8) p->streams[p->n++] = s; else SC_Release(&s->sc); }
    return S_OK;
}

/* ---------- manager ---------- */
typedef struct { IWMProfileManager iface; LONG ref; } PM;
static HRESULT WINAPI PM_QI(IWMProfileManager *t, REFIID id, void **o)
{
    if (IsEqualGUID(id, &G_IUnknown) || IsEqualGUID(id, &G_IWMProfileManager)) { *o = t; ((PM *)t)->ref++; return S_OK; }
    logmsg("ProfileManager QI %s -> E_NOINTERFACE", guidstr(id));
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI PM_AddRef(IWMProfileManager *t) { return ++((PM *)t)->ref; }
static ULONG WINAPI PM_Release(IWMProfileManager *t) { PM *p = (PM *)t; if (!--p->ref) { free(p); return 0; } return p->ref; }
static HRESULT WINAPI PM_CreateEmptyProfile(IWMProfileManager *t, WMT_VERSION v, IWMProfile **o)
{
    logmsg("PM CreateEmptyProfile(%lx)", (DWORD)v);
    *o = &new_prof()->iface; return S_OK;
}
static HRESULT WINAPI PM_LoadProfileByID(IWMProfileManager *t, REFGUID g, IWMProfile **o) { logmsg("PM LoadProfileByID %s", guidstr(g)); return E_NOTIMPL; }
static HRESULT WINAPI PM_LoadProfileByData(IWMProfileManager *t, const WCHAR *d, IWMProfile **o)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, d, -1, NULL, 0, NULL, NULL);
    char *x = malloc(len + 1);
    WideCharToMultiByte(CP_UTF8, 0, d, -1, x, len, NULL, NULL);
    x[len] = 0;
    logmsg("PM LoadProfileByData (%d bytes):\n%.4000s", len, x);
    Prof *p = new_prof();
    HRESULT hr = parse_profile(x, p);
    logmsg("   parsed %d stream(s)", p->n);
    for (int i = 0; i < p->n; i++) {
        PSC *s = p->streams[i];
        logmsg("   stream %u type=%s bitrate=%lu quality=%lu keyspace=%lld subtype=%s cbFormat=%lu", s->num, guidstr(&s->type), s->bitrate, s->quality, s->keyspace, guidstr(&s->mt.subtype), s->mt.cbFormat);
        if (IsEqualGUID(&s->type, &G_Video) && s->mt.cbFormat >= sizeof(VIH)) {
            VIH *v = (VIH *)s->fmt;
            logmsg("      video %ldx%ld avgtime=%lld", v->bmi.biWidth, v->bmi.biHeight, v->AvgTimePerFrame);
        }
    }
    free(x);
    if (FAILED(hr)) { PF_Release(&p->iface); return hr; }
    *o = &p->iface; return S_OK;
}
static HRESULT WINAPI PM_SaveProfile(IWMProfileManager *t, IWMProfile *p, WCHAR *d, DWORD *l) { logmsg("PM SaveProfile"); return E_NOTIMPL; }
static HRESULT WINAPI PM_GetSystemProfileCount(IWMProfileManager *t, DWORD *c) { *c = 0; return S_OK; }
static HRESULT WINAPI PM_LoadSystemProfile(IWMProfileManager *t, DWORD i, IWMProfile **p) { return E_INVALIDARG; }
static const IWMProfileManagerVtbl pm_vtbl = {PM_QI, PM_AddRef, PM_Release, PM_CreateEmptyProfile, PM_LoadProfileByID, PM_LoadProfileByData, PM_SaveProfile, PM_GetSystemProfileCount, PM_LoadSystemProfile};

HRESULT profile_manager_create(IWMProfileManager **pp)
{
    PM *p = calloc(1, sizeof *p);
    p->iface.lpVtbl = &pm_vtbl; p->ref = 1;
    *pp = &p->iface;
    return S_OK;
}
