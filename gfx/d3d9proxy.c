/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Proxy d3d9.dll for The Movies (32-bit, D3D9) running on DXVK/wined3d.
 * Loads the real d3d9.dll from the system directory, patches the vtable entries of IDirect3D9::CreateDevice and
 * IDirect3DDevice9::Present/Reset, and in Present runs a full-screen post-process pass (brightness, contrast,
 * saturation, gamma) on the back buffer. Settings: TheMoviesGfx.ini next to this dll (reloaded live) + hotkeys.
 * Any failure disables the effect and falls through to the real Present. No game files are modified. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "shader.h"
#include "shader_fx.h"
#include "overlay_font.h"
#include "shader_vs.h"

/* ---------------- config / log ---------------- */
typedef struct { int enabled; float bright, contrast, gamma, sat, sharpen; int fxaa; } Cfg;
static int cfg_vw, cfg_vh, cfg_pw, cfg_ph;
static float cfg_htx = -5.0f, cfg_hty = 0.0f; /* HudTextX/Y: shift of the enlarged date text (px) */
static float cfg_maskk = 0.75f, cfg_maskc = 110.5f; /* MaskAspect / MaskCenterX: the mask/arrows control above the actor list is un-stretched (x squeeze) and centred on the actor list */
static float cfg_capdx = -21.0f, cfg_capdy = -6.0f; /* CapDX/CapDY: offset of the date pill's horn piece (px at HudScale) */
static float cfg_dollk = 0.72f, cfg_dollx = 0.0f; /* DollarAspect / DollarDX: the pulsing $ of the movie list items is squeezed in x and centred on its item frame (+ DollarDX px) */
static float cfg_starsp = 1.25f; /* StarSpread: spacing factor of the star row of movie list items (stars overhang the frame by half a star) */
static int cfg_dbg; /* Dbg: 1 hides the date-pill cap, 2 hides the pill body quad, 3 hides the strip bar pieces (debug aid, live) */
static BYTE *volatile g_page_root; static float cfg_badgedx = -30.0f, cfg_bandfix = 15.0f, cfg_fieldtextdy = -9.0f, cfg_studiorow = -15.0f; /* BadgeDX / StudioBandDY: Create Your Studio fixes (game px at 2560x1440) */
static float cfg_panelh = 0.0f, cfg_paneldy = 0.0f; /* PanelH: forced height (1024x768 units) of the Create Your Studio panel, 0 = off */
static float g_money_ax = 0.0f; static volatile int g_money_noratingf; static int g_money_ext; static float g_money_extR, g_money_extP0, g_money_extP1; /* money pill zoom anchor; no-rating flag; extension bar draw state */ /* x anchor of the money pill zoom: its (clamped) right edge */
static BYTE *volatile g_money_w, *volatile g_rate_w; /* money pill widget (vt 0xd32c7c) and the studio rating widget (vt 0xd4a364, laid out in a 1024x768 parent) whose hit areas must follow the scaled visuals */
static volatile float g_mrect[4] = { 0, 0, 0, 0 }, g_rrect[4] = { 0, 0, 0, 0 };
static float g_pm_real[2], g_pm_map[2]; static int g_pm_on; static float g_pm_dx, g_pm_dy; /* money pill pointer mapping state (see pm_apply) */
static struct { unsigned long seen; float L, R, T; int have; float B, cy, Bn, cyn, tcy, tcyn; /* tcy: centre of the tick button (previous frame) */ /* frame height and X-button centre (below the top) seen in the previous frame */ } g_sd; /* small yellow-bordered dialog (3x3 nine-slice of 128 tex): geometry from the latest frame it was drawn */
static struct { float zr[4], org[4]; unsigned long frame; } g_sdb[2]; /* its two buttons: zoomed rect (what the user sees) and original rect (what the game hit-tests) */
static float cfg_smalldlg = 1.5f, cfg_smalldlgh = 1.75f, cfg_sdshift = 30.0f; /* SmallDialogShift: px the box sits lower than the button-anchored position */ /* SmallDialogScale / SmallDialogHeight: zoom of small dialogs (width) and extra vertical stretch of their frame */
typedef HRESULT (WINAPI *PF_TGLD)(IDirect3DTexture9 *, UINT, D3DSURFACE_DESC *); static PF_TGLD o_TGLD; static unsigned long g_mrt_all[64][2]; static int g_mrt_nall; static const GUID g_iid_tex9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } }; static IDirect3DSurface9 *g_mrt_surf; static void mrt_seq(const char *w); static void mrt_dump2(IDirect3DDevice9 *d, IDirect3DSurface9 *src, const char *trig, const char *outn); static int g_mrt_syncdraw; static int g_dd_n0; static void mrt_seq(const char *w); static int g_mrt_movie, g_mrt_passno = -1, g_mrt_moviepass = -2; static unsigned long g_mrt_passframe; static int g_mrt_dumpclear; static float g_mrt_pd[48][5]; static int g_mrt_npd; static void *g_mrt_tex; static int g_skipvp; static IDirect3DDevice9 *g_mrt_dev; static int cfg_mrtskip = -1, cfg_mrtstop = -1, g_mrt_idx; static struct { unsigned long seen; float L, R, T, B, Ln, Rn, Tn, Bn; } g_bd; /* frame of a big dialog (extent of its 32 px high nine-slice pieces, previous frame) */ static float cfg_btnsize = 72.0f, cfg_btninset = 26.0f; /* ButtonSize / ButtonInset: confirm/cancel buttons of the scaled dialogs */ static float cfg_mrts = 1.0f; static int g_mrt; static UINT g_mrt_px; /* movie render target (stock 512x512, the movie is a 512x284 crop of it) is created cfg_mrts times bigger; g_mrt: the current render target is that one */ static int g_dbg_nodepth, g_dbg_nohooks, g_dbg_nodip; unsigned long g_mr_res = 512; volatile unsigned long g_mr_always; static unsigned long g_exp_w[5] = { 428, 640, 854, 1280, 1920 }; /* export capture width per quality level 0..4 = Upload, Small, Medium, High, Best (height = width*9/16) */ static int g_exp_on; static unsigned long g_pl_sig; static int cfg_plbottom; static float cfg_textdesc = 1.0f; /* Minimal=1 (default): all UI scaling / aspect correction / maker / player code is switched off, only the movie resolution + export + colour post-process remain */ static float cfg_plbtn = 1.0f; static float g_plb_delta = 160.0f; static float cfg_mkasp = 0.75f, cfg_mkpillc = 68.0f, cfg_mktext = 0.75f, cfg_mktbox = 1.34f, cfg_mkdesc = 4.5f, cfg_mktitledesc = 2.2f, cfg_mkthumb = 0.75f, cfg_descall = 1.0f; static float cfg_pscale = 1.5f; /* Movie Player screen: last frame its bottom band was drawn; PlayerScale = size factor of its HUD */ static unsigned long g_sdbtn; static unsigned long g_sdbig; /* last frame with a tall 128-tex frame side piece: a big dialog (Select Movie ...), not a small one */ static float cfg_loadtext = 1.6f; static unsigned long g_load_frame, g_bar_frame, g_page_frame, g_page_sig; /* g_page_sig: last frame in which a whitelisted front-end page's signature quad was drawn */ /* g_page_frame: frame in which a 4:3-designed front-end page (New Game / Create Your Studio) was detected */ /* LoadingText: size factor of the loading-screen tip text (the 4:3 loading screen is pillarboxed) */
static float cfg_bscale = 1.0f; /* BubbleScale: pop-up bubbles (tutorial / info bubbles, hover pills) */
static float cfg_dscale = 1.0f; /* DialogScale: scales options/dialog panels through the widget layout */
static float cfg_hscale = 1.0f; /* HudScale: scales the time-control widgets' layout inputs in the game itself */
static float cfg_hud = 1.0f, cfg_bar = 1.0f, cfg_mx = 0.0f, cfg_my = 0.0f; /* HudMouseX/Y: hotspot trim (game px) applied inside the zoomed block */ /* HudZoom: time-control block zoomed about the screen corner; BarZoom: timeline strip height */
static float cfg_uiscale = 1.0f; static int cfg_aspectfix; /* per-sprite UI zoom / widescreen aspect fix */ /* virtual (game layout) size, real back buffer size (0 = desktop) */
static Cfg g_cfg = {1, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0};
static char g_ini[MAX_PATH], g_logp[MAX_PATH];
static FILETIME g_ini_time;
static DWORD g_dirty_tick;
static int g_log_on = 1;

static void logmsg(const char *fmt, ...)
{
    FILE *f; va_list ap;
    if (!g_log_on || !g_logp[0]) return;
    f = fopen(g_logp, "a"); if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
static int cfg_textdebug; static float cfg_mrtcpu;
static float getf(const char *key, float def)
{
    char b[64]; GetPrivateProfileStringA("Gfx", key, "", b, sizeof b, g_ini);
    return b[0] ? (float)atof(b) : def;
}
static int file_time(FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(g_ini, GetFileExInfoStandard, &d)) return 0;
    *ft = d.ftLastWriteTime; return 1;
}
static void load_ini(void)
{
    g_cfg.enabled = (int)getf("Enabled", 1);
    g_cfg.bright = getf("Brightness", 0.0f);
    g_cfg.contrast = getf("Contrast", 1.0f);
    g_cfg.gamma = getf("Gamma", 1.0f);
    g_cfg.sat = getf("Saturation", 1.0f);
    g_cfg.sharpen = getf("Sharpen", 0.0f); if (g_cfg.sharpen < 0.0f) g_cfg.sharpen = 0.0f; if (g_cfg.sharpen > 1.0f) g_cfg.sharpen = 1.0f; g_cfg.fxaa = (getf("FXAA", 0.0f) > 0.5f);
    g_log_on = (int)getf("Log", 1);
    cfg_vw = (int)getf("VirtualWidth", 0); cfg_vh = (int)getf("VirtualHeight", 0);
    cfg_pw = (int)getf("RealWidth", 0); cfg_ph = (int)getf("RealHeight", 0);
    g_mrt_syncdraw = (getf("MrtSyncDraw", 0.0f) > 0.5f);
    g_dbg_nodepth = (getf("DbgNoDepth", 0.0f) > 0.5f); g_dbg_nohooks = (getf("DbgNoRtHooks", 0.0f) > 0.5f); g_dbg_nodip = (getf("DbgNoDipWrap", 0.0f) > 0.5f);
    g_mr_always = (getf("MovieResEverywhere", 0.0f) > 0.5f); g_exp_on = (getf("ExportPresets", 1.0f) > 0.5f); { float v = getf("ExportWidth", 1920.0f); if (v >= 160.0f && v <= 3840.0f) g_exp_w[4] = (unsigned long)(v + 0.5f); }
    cfg_mrtskip = (int)getf("MrtSkip", -1.0f); cfg_mrtstop = (int)getf("MrtStop", -1.0f);
    cfg_mrts = getf("MovieRTScale", 1.0f); if (cfg_mrts < 1.0f) cfg_mrts = 1.0f; if (cfg_mrts > 4.0f) cfg_mrts = 4.0f; g_mrt_px = (UINT)(512.0f * cfg_mrts + 0.5f);
    cfg_textdesc = getf("TextDescent", 1.0f);
    cfg_mrtcpu = getf("MrtCPU", 0.0f);
    cfg_textdebug = getf("TextDebug", 0.0f) > 0.5f; /* read once: getf() re-reads the ini file from disk on every call */
    cfg_dbg = (int)getf("Dbg", 0);
    if (cfg_uiscale < 0.5f) cfg_uiscale = 0.5f; if (cfg_uiscale > 3.0f) cfg_uiscale = 3.0f;
    if (g_cfg.gamma < 0.2f) g_cfg.gamma = 0.2f;
    file_time(&g_ini_time);
}
static void save_ini(void)
{
    char b[64]; const char *s = "Gfx";
    sprintf(b, "%d", g_cfg.enabled); WritePrivateProfileStringA(s, "Enabled", b, g_ini);
    sprintf(b, "%.3f", g_cfg.bright); WritePrivateProfileStringA(s, "Brightness", b, g_ini);
    sprintf(b, "%.3f", g_cfg.contrast); WritePrivateProfileStringA(s, "Contrast", b, g_ini);
    sprintf(b, "%.3f", g_cfg.gamma); WritePrivateProfileStringA(s, "Gamma", b, g_ini);
    sprintf(b, "%.3f", g_cfg.sat); WritePrivateProfileStringA(s, "Saturation", b, g_ini);
    sprintf(b, "%.3f", g_cfg.sharpen); WritePrivateProfileStringA(s, "Sharpen", b, g_ini); sprintf(b, "%d", g_cfg.fxaa); WritePrivateProfileStringA(s, "FXAA", b, g_ini);
    sprintf(b, "%.3f", cfg_uiscale); WritePrivateProfileStringA(s, "UiScale", b, g_ini);
    sprintf(b, "%d", cfg_aspectfix); WritePrivateProfileStringA(s, "UiAspectFix", b, g_ini);
    sprintf(b, "%.3f", cfg_hud); WritePrivateProfileStringA(s, "HudZoom", b, g_ini);
    sprintf(b, "%.3f", cfg_bar); WritePrivateProfileStringA(s, "BarZoom", b, g_ini);
    sprintf(b, "%.3f", cfg_hscale); WritePrivateProfileStringA(s, "HudScale", b, g_ini);
    sprintf(b, "%.3f", cfg_dscale); WritePrivateProfileStringA(s, "DialogScale", b, g_ini);
    sprintf(b, "%.3f", cfg_bscale); WritePrivateProfileStringA(s, "BubbleScale", b, g_ini);
    sprintf(b, "%.3f", cfg_maskk); WritePrivateProfileStringA(s, "MaskAspect", b, g_ini);
    sprintf(b, "%.2f", cfg_maskc); WritePrivateProfileStringA(s, "MaskCenterX", b, g_ini);
    sprintf(b, "%.3f", cfg_htx); WritePrivateProfileStringA(s, "HudTextX", b, g_ini);
    sprintf(b, "%.3f", cfg_hty); WritePrivateProfileStringA(s, "HudTextY", b, g_ini);
    sprintf(b, "%.3f", cfg_mx); WritePrivateProfileStringA(s, "HudMouseX", b, g_ini);
    sprintf(b, "%.3f", cfg_my); WritePrivateProfileStringA(s, "HudMouseY", b, g_ini);
    file_time(&g_ini_time);
    logmsg("saved: bright=%.3f contrast=%.3f gamma=%.3f sat=%.3f enabled=%d", g_cfg.bright, g_cfg.contrast, g_cfg.gamma, g_cfg.sat, g_cfg.enabled);
}
static void write_default_ini(void)
{
    FILE *f = fopen(g_ini, "r");
    if (f) { fclose(f); return; }
    f = fopen(g_ini, "w"); if (!f) return;
    fputs("; The Movies post-process settings (reloaded live while the game runs; hotkeys edit and save this file).\r\n"
          "; Brightness: -0.5..0.5 (0 = unchanged)   Contrast: 0.5..2 (1 = unchanged)\r\n"
          "; Gamma: 0.3..3 (1 = unchanged; <1 darkens midtones, >1 brightens)   Saturation: 0..2 (1 = unchanged)\r\n"
          "; Hotkeys (hold Ctrl+Shift): Left/Right brightness, Down/Up gamma, PgDn/PgUp contrast, End/Home saturation,\r\n"
          ";   Insert on/off, Backspace reset, Enter save now (changes also auto-save after 2 s).\r\n"
          "; UI scaling: set the game resolution (registry GD: Screen Width/Height) to the VIRTUAL size, put the same numbers here;\r\n"
          "; the proxy then gives the game a RealWidth x RealHeight back buffer (0 = desktop size) and scales the UI up. 0 = off.\r\n"
          "; UI sprites: the game stretches its 4:3 (1600x1200) UI to the screen, so on 16:9 sprites/text come out too wide. UiAspectFix=1 restores their proportions,\r\n"
          "; UiScale enlarges every UI sprite about its own centre (1.0 = off). Large panels/backgrounds are never touched. Hotkeys (Ctrl+Shift): F8 aspect fix on/off, F9/F10 UiScale -/+.\r\n"
          "[Gfx]\r\nEnabled=1\r\nBrightness=0.000\r\nContrast=1.000\r\nGamma=1.000\r\nSaturation=1.000\r\nUiScale=1.000\r\nUiAspectFix=0\r\nLog=1\r\nVirtualWidth=0\r\nVirtualHeight=0\r\nRealWidth=0\r\nRealHeight=0\r\n", f);
    fclose(f);
}

/* ---------------- real d3d9 ---------------- */
typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT (WINAPI *PFN_Create9Ex)(UINT, IDirect3D9Ex **);
static HMODULE g_real;
static PFN_Create9 real_Create9;
static PFN_Create9Ex real_Create9Ex;
typedef HRESULT (WINAPI *PFN_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (WINAPI *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static PFN_CreateDevice orig_CreateDevice;
static PFN_Present orig_Present;
static PFN_Reset orig_Reset;

static int load_real(void)
{
    WCHAR p[MAX_PATH];
    if (g_real) return 1;
    GetSystemDirectoryW(p, MAX_PATH); lstrcatW(p, L"\\d3d9.dll");
    g_real = LoadLibraryW(p);
    if (!g_real) { logmsg("cannot load real d3d9.dll"); return 0; }
    real_Create9 = (PFN_Create9)GetProcAddress(g_real, "Direct3DCreate9");
    real_Create9Ex = (PFN_Create9Ex)GetProcAddress(g_real, "Direct3DCreate9Ex");
    logmsg("real d3d9.dll loaded (%p)", g_real);
    return real_Create9 != NULL;
}
static void *patch_vt(void *obj, int idx, void *hook)
{
    void **vt = *(void ***)obj; void *old; DWORD prot;
    if (vt[idx] == hook) return NULL;
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &prot);
    old = vt[idx]; vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void *), prot, &prot);
    return old;
}

/* ---------------- post-process resources ---------------- */
static struct {
    IDirect3DDevice9 *dev; HWND hwnd;
    IDirect3DTexture9 *tex; IDirect3DPixelShader9 *ps, *ps_fx; IDirect3DStateBlock9 *sb;
    UINT w, h; D3DFORMAT fmt; int failed; unsigned long frame; int logged;
} G;

static void res_release(void)
{
    if (G.tex) { IDirect3DTexture9_Release(G.tex); G.tex = NULL; }
    if (G.sb) { IDirect3DStateBlock9_Release(G.sb); G.sb = NULL; }
    G.w = G.h = 0;
}
static int res_ensure(IDirect3DDevice9 *dev, const D3DSURFACE_DESC *d)
{
    HRESULT hr;
    if (G.tex && (G.w != d->Width || G.h != d->Height || G.fmt != d->Format)) res_release();
    if (!G.ps) {
        hr = IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)g_ps, &G.ps);
        if (FAILED(hr)) { logmsg("CreatePixelShader failed %lx", hr); return 0; }
    }
    if (!G.ps_fx && (g_cfg.fxaa || g_cfg.sharpen > 0.0f)) { if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)g_ps_fx, &G.ps_fx))) { logmsg("CreatePixelShader (fx) failed"); G.ps_fx = NULL; } }
    if (!G.tex) {
        hr = IDirect3DDevice9_CreateTexture(dev, d->Width, d->Height, 1, D3DUSAGE_RENDERTARGET, d->Format, D3DPOOL_DEFAULT, &G.tex, NULL);
        if (FAILED(hr)) { logmsg("CreateTexture %ux%u fmt %d failed %lx", d->Width, d->Height, d->Format, hr); return 0; }
        G.w = d->Width; G.h = d->Height; G.fmt = d->Format;
        logmsg("post-process target %ux%u fmt %d", G.w, G.h, G.fmt);
    }
    if (!G.sb) {
        hr = IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &G.sb);
        if (FAILED(hr)) { logmsg("CreateStateBlock failed %lx", hr); return 0; }
    }
    return 1;
}

static int g_in_post;
static HRESULT real_desc(IDirect3DSurface9 *s, D3DSURFACE_DESC *d);
typedef struct { float x, y, z, rhw, u, v; } QV;
static void post_process(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL, *ts = NULL, *rt0 = NULL;
    D3DSURFACE_DESC d; QV q[4]; float k[4]; HRESULT hr;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    real_desc(bb, &d);
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE && 0) goto done;
    g_in_post = 1;
    if (!res_ensure(dev, &d)) { G.failed++; goto done; }
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(G.tex, 0, &ts))) goto done;
    hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, ts, NULL, D3DTEXF_NONE);
    if (FAILED(hr)) { logmsg("StretchRect failed %lx", hr); G.failed++; goto done; }
    IDirect3DStateBlock9_Capture(G.sb);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt0);
    if (rt0 != bb) IDirect3DDevice9_SetRenderTarget(dev, 0, bb);

    k[0] = g_cfg.bright; k[1] = g_cfg.contrast; k[2] = 1.0f / g_cfg.gamma; k[3] = g_cfg.sat;
    q[0] = (QV){-0.5f, -0.5f, 0, 1, 0, 0};
    q[1] = (QV){d.Width - 0.5f, -0.5f, 0, 1, 1, 0};
    q[2] = (QV){-0.5f, d.Height - 0.5f, 0, 1, 0, 1};
    q[3] = (QV){d.Width - 0.5f, d.Height - 0.5f, 0, 1, 1, 1};
    {
        D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
        IDirect3DDevice9_SetViewport(dev, &vp);
    }
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, (g_cfg.fxaa || g_cfg.sharpen > 0.0f) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, (g_cfg.fxaa || g_cfg.sharpen > 0.0f) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)G.tex);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetPixelShader(dev, ((g_cfg.fxaa || g_cfg.sharpen > 0.0f) && G.ps_fx) ? G.ps_fx : G.ps);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, k, 1);
    { float p1[4]; p1[0] = 1.0f / (float)d.Width; p1[1] = 1.0f / (float)d.Height; p1[2] = g_cfg.sharpen; p1[3] = g_cfg.fxaa ? 1.0f : 0.0f; IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, p1, 1); }
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
        IDirect3DDevice9_EndScene(dev);
        if (FAILED(hr)) { logmsg("DrawPrimitiveUP failed %lx", hr); G.failed++; }
        else if (!G.logged) { G.logged = 1; logmsg("first post-process frame drawn OK"); }
    }
    IDirect3DStateBlock9_Apply(G.sb);
    if (rt0 != bb && rt0) IDirect3DDevice9_SetRenderTarget(dev, 0, rt0);
done:
    g_in_post = 0;
    if (rt0) IDirect3DSurface9_Release(rt0);
    if (ts) IDirect3DSurface9_Release(ts);
    if (bb) IDirect3DSurface9_Release(bb);
}

/* ---------------- hotkeys ---------------- */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
/* ---------------- in-game settings overlay (Ctrl+Shift+F7 toggles; Ctrl+Shift+Up/Down select, Left/Right change) ---------------- */
static int g_ov_open, g_ov_sel; static IDirect3DTexture9 *g_ov_tex; static IDirect3DStateBlock9 *g_ov_sb;
typedef struct { const char *name; int restart; } OvItem;
static const OvItem g_ovitems[] = { {"Post-processing", 0}, {"Brightness", 0}, {"Contrast", 0}, {"Gamma", 0}, {"Saturation", 0}, {"Sharpen", 0}, {"FXAA", 0},
    {"MSAA", 1}, {"Anisotropic filtering", 1}, {"Texture LOD bias", 1}, {"Movie resolution", 1}, {"Text tail fix", 1} };
#define OV_N ((int)(sizeof g_ovitems / sizeof g_ovitems[0]))
static void ov_dxvk_path(char *out) { strcpy(out, g_ini); { char *sl = strrchr(out, '\\'); if (sl) strcpy(sl + 1, "dxvk.conf"); else strcpy(out, "dxvk.conf"); } }
static float ov_dxvk_get(const char *key, float def)
{
    char path[MAX_PATH], line[256]; FILE *f; float v = def; ov_dxvk_path(path); f = fopen(path, "r"); if (!f) return def;
    while (fgets(line, sizeof line, f)) { char k[128]; float x; if (sscanf(line, " %127[^= ] = %f", k, &x) == 2 && !strcmp(k, key)) v = x; }
    fclose(f); return v;
}
static void ov_dxvk_set(const char *key, float v, int remove)
{
    char path[MAX_PATH], line[256], keep[40][256]; int n = 0, i; FILE *f; ov_dxvk_path(path);
    if ((f = fopen(path, "r"))) { while (n < 38 && fgets(line, sizeof line, f)) { char k[128]; if (sscanf(line, " %127[^= ] =", k) == 1 && !strcmp(k, key)) continue; if (line[0] != '\n') strcpy(keep[n++], line); } fclose(f); }
    if (!remove) sprintf(keep[n++], "%s = %g\n", key, v);
    if ((f = fopen(path, "w"))) { for (i = 0; i < n; i++) fputs(keep[i], f); fclose(f); }
}
static void ov_ini_set(const char *key, const char *val) { WritePrivateProfileStringA("Gfx", key, val, g_ini); }
static void ov_value(int i, char *out)
{
    static const int ms[4] = {0, 2, 4, 8}, af[5] = {0, 2, 4, 8, 16}; int q;
    switch (i) {
    case 0: strcpy(out, g_cfg.enabled ? "On" : "Off"); break;
    case 1: sprintf(out, "%+.2f", g_cfg.bright); break;
    case 2: sprintf(out, "%.2f", g_cfg.contrast); break;
    case 3: sprintf(out, "%.2f", g_cfg.gamma); break;
    case 4: sprintf(out, "%.2f", g_cfg.sat); break;
    case 5: sprintf(out, "%.2f", g_cfg.sharpen); break;
    case 6: strcpy(out, g_cfg.fxaa ? "On" : "Off"); break;
    case 7: q = (int)getf("MSAA", 0.0f); if (q < 2) strcpy(out, "Off"); else sprintf(out, "%dx", q); (void)ms; break;
    case 8: q = (int)ov_dxvk_get("d3d9.samplerAnisotropy", 0.0f); if (q < 2) strcpy(out, "Game default"); else sprintf(out, "%dx", q); (void)af; break;
    case 9: sprintf(out, "%+.1f", ov_dxvk_get("d3d9.samplerLodBias", 0.0f)); break;
    case 10: sprintf(out, "%d", (int)getf("MovieRes", 1024.0f)); break;
    case 11: strcpy(out, getf("TextDescent", 1.0f) > 0.5f ? "On" : "Off"); break;
    default: out[0] = 0;
    }
}
static int ov_step_list(int cur, const int *list, int n, int dir) { int i, idx = 0; for (i = 0; i < n; i++) if (list[i] <= cur) idx = i; idx += dir; if (idx < 0) idx = 0; if (idx >= n) idx = n - 1; return list[idx]; }
static void ov_adjust(int i, int dir)
{
    static const int ms[4] = {0, 2, 4, 8}, af[5] = {0, 2, 4, 8, 16}, mr[3] = {512, 1024, 2048}; char b[32]; int q;
    switch (i) {
    case 0: g_cfg.enabled = !g_cfg.enabled; break;
    case 1: g_cfg.bright = clampf(g_cfg.bright + 0.01f * dir, -0.5f, 0.5f); break;
    case 2: g_cfg.contrast = clampf(g_cfg.contrast + 0.02f * dir, 0.5f, 2.0f); break;
    case 3: g_cfg.gamma = clampf(g_cfg.gamma + 0.02f * dir, 0.3f, 3.0f); break;
    case 4: g_cfg.sat = clampf(g_cfg.sat + 0.02f * dir, 0.0f, 2.0f); break;
    case 5: g_cfg.sharpen = clampf(g_cfg.sharpen + 0.05f * dir, 0.0f, 1.0f); break;
    case 6: g_cfg.fxaa = !g_cfg.fxaa; break;
    case 7: q = ov_step_list((int)getf("MSAA", 0.0f), ms, 4, dir); sprintf(b, "%d", q); ov_ini_set("MSAA", b); break;
    case 8: q = ov_step_list((int)ov_dxvk_get("d3d9.samplerAnisotropy", 0.0f), af, 5, dir); if (q < 2) ov_dxvk_set("d3d9.samplerAnisotropy", 0, 1); else ov_dxvk_set("d3d9.samplerAnisotropy", (float)q, 0); break;
    case 9: { float v = ov_dxvk_get("d3d9.samplerLodBias", 0.0f) + 0.1f * dir; if (v < -2.0f) v = -2.0f; if (v > 1.0f) v = 1.0f; if (fabsf(v) < 0.05f) ov_dxvk_set("d3d9.samplerLodBias", 0, 1); else ov_dxvk_set("d3d9.samplerLodBias", v, 0); break; }
    case 10: q = ov_step_list((int)getf("MovieRes", 1024.0f), mr, 3, dir); sprintf(b, "%d", q); ov_ini_set("MovieRes", b); break;
    case 11: ov_ini_set("TextDescent", getf("TextDescent", 1.0f) > 0.5f ? "0" : "1"); break;
    }
    if (i <= 6) g_dirty_tick = GetTickCount() | 1;
}
static void ov_nav(void)
{
    static int held[4], prev_f7; int vk[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT}, k;
    for (k = 0; k < 4; k++) {
        int d = key(vk[k]); int fire = 0;
        if (d) { held[k]++; if (held[k] == 1 || (held[k] > 14 && held[k] % 3 == 0 && k >= 2)) fire = 1; } else held[k] = 0;
        if (!fire) continue;
        if (k == 0) g_ov_sel = (g_ov_sel + OV_N - 1) % OV_N; else if (k == 1) g_ov_sel = (g_ov_sel + 1) % OV_N; else ov_adjust(g_ov_sel, k == 2 ? -1 : 1);
    }
    (void)prev_f7;
}
typedef struct { float x, y, z, rhw; DWORD c; float u, v; } OV;
static OV g_ovv[6 * 2048], g_ovbg[6 * 8]; static int g_novv, g_nbg;
static void ov_rect(float x0, float y0, float x1, float y1, DWORD c)
{
    OV *q; if (g_nbg + 6 > (int)(sizeof g_ovbg / sizeof g_ovbg[0])) return; q = g_ovbg + g_nbg; g_nbg += 6;
    q[0] = (OV){x0, y0, 0, 1, c, 0, 0}; q[1] = (OV){x1, y0, 0, 1, c, 0, 0}; q[2] = (OV){x0, y1, 0, 1, c, 0, 0}; q[3] = (OV){x1, y0, 0, 1, c, 0, 0}; q[4] = (OV){x1, y1, 0, 1, c, 0, 0}; q[5] = (OV){x0, y1, 0, 1, c, 0, 0};
}
static void ov_quad(float x0, float y0, float x1, float y1, DWORD c, float u0, float v0, float u1, float v1)
{
    OV *q; if (g_novv + 6 > (int)(sizeof g_ovv / sizeof g_ovv[0])) return; q = g_ovv + g_novv; g_novv += 6;
    q[0] = (OV){x0, y0, 0, 1, c, u0, v0}; q[1] = (OV){x1, y0, 0, 1, c, u1, v0}; q[2] = (OV){x0, y1, 0, 1, c, u0, v1};
    q[3] = (OV){x1, y0, 0, 1, c, u1, v0}; q[4] = (OV){x1, y1, 0, 1, c, u1, v1}; q[5] = (OV){x0, y1, 0, 1, c, u0, v1};
}
static void ov_text(float x, float y, float sc, const char *t, DWORD c)
{
    for (; *t; t++, x += 8 * sc) { int ch = (unsigned char)*t - 32; float u, v; if (ch < 0 || ch > 94 || ch == 0) continue; u = (ch % 16) * 8 / 128.0f; v = (ch / 16) * 16 / 96.0f; ov_quad(x, y, x + 8 * sc, y + 16 * sc, c, u, v, u + 8 / 128.0f, v + 16 / 96.0f); }
}
static void ov_draw(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL, *rt0 = NULL; D3DSURFACE_DESC d; float sc, x0, y0, w, h, ly; int i; char line[96], val[40]; D3DVIEWPORT9 vp;
    if (!g_ov_open) return;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    real_desc(bb, &d); g_in_post = 1;
    if (!g_ov_tex) {
        D3DLOCKED_RECT lr; UINT y, x;
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, 128, 96, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_ov_tex, NULL)) || !g_ov_tex) { g_ov_tex = NULL; goto done; }
        if (SUCCEEDED(IDirect3DTexture9_LockRect(g_ov_tex, 0, &lr, NULL, 0))) { for (y = 0; y < 96; y++) for (x = 0; x < 128; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = g_ovfont[y * 128 + x] ? 0xffffffffu : 0x00ffffffu; IDirect3DTexture9_UnlockRect(g_ov_tex, 0); }
    }
    if (!g_ov_sb && FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &g_ov_sb))) { g_ov_sb = NULL; goto done; }
    sc = (float)(d.Height / 720); if (sc < 1.0f) sc = 1.0f;
    x0 = 24.0f * sc; y0 = 24.0f * sc; w = 8 * sc * 52; h = (OV_N + 4) * 16 * sc * 1.25f + 8 * sc;
    g_novv = 0; g_nbg = 0;
    ov_rect(x0, y0, x0 + w, y0 + h, 0xd0101820u);
    ly = y0 + 6 * sc;
    ov_text(x0 + 8 * sc, ly, sc, "GRAPHICS SETTINGS   Ctrl+Shift+F7 close", 0xffffd060u); ly += 16 * sc * 1.25f * 1.3f;
    for (i = 0; i < OV_N; i++) {
        DWORD c = (i == g_ov_sel) ? 0xffffffffu : 0xffb0c0d0u; ov_value(i, val);
        if (i == g_ov_sel) ov_rect(x0 + 4 * sc, ly - 1 * sc, x0 + w - 4 * sc, ly + 16 * sc * 1.1f, 0x60406080u);
        sprintf(line, "%c %-23s %-14s%s", i == g_ov_sel ? '>' : ' ', g_ovitems[i].name, val, g_ovitems[i].restart ? " (restart)" : "");
        ov_text(x0 + 8 * sc, ly, sc, line, c); ly += 16 * sc * 1.25f;
    }
    ly += 6 * sc; ov_text(x0 + 8 * sc, ly, sc, "Ctrl+Shift+Up/Down select, Left/Right change", 0xff90a0b0u);
    IDirect3DStateBlock9_Capture(g_ov_sb);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt0); if (rt0 != bb) IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    vp = (D3DVIEWPORT9){0, 0, d.Width, d.Height, 0.0f, 1.0f}; IDirect3DDevice9_SetViewport(dev, &vp);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE); IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF); IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0); IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE); IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetVertexShader(dev, NULL); IDirect3DDevice9_SetPixelShader(dev, NULL); IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
        IDirect3DDevice9_SetTexture(dev, 0, NULL); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        if (g_nbg) IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, g_nbg / 3, g_ovbg, sizeof(OV));
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_ov_tex); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        if (g_novv) IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, g_novv / 3, g_ovv, sizeof(OV));
        IDirect3DDevice9_EndScene(dev);
    }
    IDirect3DStateBlock9_Apply(g_ov_sb);
    if (rt0 != bb && rt0) IDirect3DDevice9_SetRenderTarget(dev, 0, rt0);
done:
    g_in_post = 0; if (rt0) IDirect3DSurface9_Release(rt0); if (bb) IDirect3DSurface9_Release(bb);
}
static void hotkeys(void)
{
    static int prev_ins, prev_bs, prev_ret, prev_f8; int ins, bs, ret, f8, changed = 0; float step;
    if (!(key(VK_CONTROL) && key(VK_SHIFT))) { prev_ins = prev_bs = prev_ret = 0; return; }
    if (G.hwnd && GetForegroundWindow() != G.hwnd) return;
    { static int pf7; int f7 = key(VK_F7); if (f7 && !pf7) g_ov_open = !g_ov_open; pf7 = f7; }
    if (g_ov_open) { ov_nav(); return; } /* the overlay owns the Ctrl+Shift keys while it is open */
    if (G.frame % 3 == 0) {
        step = 0.01f;
        if (key(VK_LEFT)) { g_cfg.bright -= step; changed = 1; }
        if (key(VK_RIGHT)) { g_cfg.bright += step; changed = 1; }
        if (key(VK_DOWN)) { g_cfg.gamma -= 0.02f; changed = 1; }
        if (key(VK_UP)) { g_cfg.gamma += 0.02f; changed = 1; }
        if (key(VK_NEXT)) { g_cfg.contrast -= 0.02f; changed = 1; }
        if (key(VK_PRIOR)) { g_cfg.contrast += 0.02f; changed = 1; }
        if (key(VK_END)) { g_cfg.sat -= 0.02f; changed = 1; }
        if (key(VK_HOME)) { g_cfg.sat += 0.02f; changed = 1; }
        if (key(VK_F9)) { cfg_uiscale -= 0.05f; changed = 1; }
        if (key(VK_F10)) { cfg_uiscale += 0.05f; changed = 1; }
    }
    f8 = key(VK_F8); if (f8 && !prev_f8) { cfg_aspectfix = !cfg_aspectfix; changed = 1; logmsg("UiAspectFix=%d", cfg_aspectfix); } prev_f8 = f8;
    ins = key(VK_INSERT); bs = key(VK_BACK); ret = key(VK_RETURN);
    if (ins && !prev_ins) { g_cfg.enabled = !g_cfg.enabled; changed = 1; }
    if (bs && !prev_bs) { g_cfg.bright = 0; g_cfg.contrast = 1; g_cfg.gamma = 1; g_cfg.sat = 1; cfg_uiscale = 1.0f; cfg_aspectfix = 0; changed = 1; }
    if (ret && !prev_ret) save_ini();
    prev_ins = ins; prev_bs = bs; prev_ret = ret;
    if (changed) {
        g_cfg.bright = clampf(g_cfg.bright, -0.5f, 0.5f); g_cfg.contrast = clampf(g_cfg.contrast, 0.5f, 2.0f);
        g_cfg.gamma = clampf(g_cfg.gamma, 0.3f, 3.0f); g_cfg.sat = clampf(g_cfg.sat, 0.0f, 2.0f); cfg_uiscale = clampf(cfg_uiscale, 0.5f, 3.0f);
        g_dirty_tick = GetTickCount() | 1;
    }
}



/* ---------------- virtual resolution (UI scaling) ---------------- */
/* The game lays its UI out for VW x VH (pre-transformed, screen-space vertices) while we give it a PW x PH back buffer.
 * 3D needs nothing (default viewport = whole back buffer, projection aspect is the same). Pre-transformed draws are
 * re-routed through a tiny vertex shader that maps virtual pixel coordinates to clip space. */
static float g_cur_x, g_cur_y; static int g_cur_ok; /* raw (game layout) top-left of the last small quad drawn */
static BYTE *g_wptr_date(void);
static float g_dxtra = 1.0f; /* extra zoom factor for the draw being prepared (panel title badge) */
static int dialog_text_anchor(const float *b, float *cx, float *cy);
static int dialog_band_fit(const float *b, float *ay, float *ky);
static int player_text_anchor(const float *b, float *ax, float *ay);
static void dialog_mark_visible(const float *b);
static BYTE *g_visroot; static unsigned long g_visframe; static int g_dutex; /* the batch being anchored is untextured (panel frame pieces, not text) */
static struct {
    int on, valid; UINT vw, vh, pw, ph; int rt_bb; DWORD fvf; IDirect3DDevice9 *dev; unsigned long zoomed, exempt;
    IDirect3DVertexShader9 *vs_c, *vs_n; struct { DWORD fvf; IDirect3DVertexDeclaration9 *d; } decl[8]; int ndecl;
    unsigned long scaled, skipped, calls_s2c, calls_c2s, msgs;
} V;
static int ui_enabled(void) { return 0; }
#define HUD_RX 260.0f /* time-control block (date pill, speed buttons) in 2560x1440 px: x 0..256, y 0..64 */
#define HUD_RY 66.0f
#define BAR_Y 33.0f   /* timeline strip: y 0..16, its 32 px period markers reach y 32 */
#define BAR_BOTTOM 16.0f /* the timeline strip ends here; the time-control block and the money pill hang from it, so vertical positions scale about this line */
static int rhw_fvf(DWORD f) { return (f & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW; }
static int rhw_supported(DWORD f)
{
    int tc = (f & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (f & ~(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEXCOUNT_MASK)) return 0; /* specular, normals, 3-float texcoords ... */
    return tc <= 2;
}
static IDirect3DVertexDeclaration9 *v_decl(IDirect3DDevice9 *dev, DWORD fvf)
{
    D3DVERTEXELEMENT9 el[6]; int n = 0, i, tc = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT; WORD off = 16;
    IDirect3DVertexDeclaration9 *d = NULL; D3DVERTEXELEMENT9 end = D3DDECL_END();
    for (i = 0; i < V.ndecl; i++) if (V.decl[i].fvf == fvf) return V.decl[i].d;
    el[n++] = (D3DVERTEXELEMENT9){0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0};
    if (fvf & D3DFVF_DIFFUSE) { el[n++] = (D3DVERTEXELEMENT9){0, off, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}; off += 4; }
    for (i = 0; i < tc; i++) { el[n++] = (D3DVERTEXELEMENT9){0, off, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, (BYTE)i}; off += 8; }
    el[n++] = end;
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, el, &d))) return NULL;
    if (V.ndecl < 8) { V.decl[V.ndecl].fvf = fvf; V.decl[V.ndecl].d = d; V.ndecl++; }
    return d;
}
static float g_bigtip[4][2]; static unsigned long g_bigf[4]; static int g_nbig;
static UINT tex0_width(IDirect3DDevice9 *dev)
{
    IDirect3DBaseTexture9 *bt = NULL; UINT w = 0;
    if (SUCCEEDED(IDirect3DDevice9_GetTexture(dev, 0, &bt)) && bt) { if (IDirect3DBaseTexture9_GetType(bt) == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC d; IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)bt, 0, &d); w = d.Width; } IDirect3DBaseTexture9_Release(bt); }
    return w;
}
/* Loading-screen tip text: the game wraps the tip for the stretched panel and draws every glyph run as one batch. Re-wrap: runs of the previous frame (same order every frame) are laid out again in a
 * wider scale (Sx = 0.75*T across, T down) into lines that fill the 4:3 panel, block centred, lines left aligned. Everything in 2560x1440 px units. */
#define LT_MAX 400
static struct { float x0, x1, y0; } lt_cur[LT_MAX], lt_prev[LT_MAX]; static float lt_nx[LT_MAX], lt_ny[LT_MAX]; static int lt_ncur, lt_nprev, lt_ok; static unsigned long lt_frame; static float lt_T;
static unsigned long g_fd_on, g_cast_frame, g_mk_arrow, g_mk_bd, g_mk_sig = 1000000000UL;
static volatile int g_atlas_arm, g_thumb_arm2;
static int v_prepare(IDirect3DDevice9 *dev, DWORD fvf, const float *b, UINT nv)
{
    IDirect3DVertexDeclaration9 *d; IDirect3DVertexShader9 **pvs; float k[4], a[4] = {0, 0, 1, 1};
    if (!g_mrt) return 0; /* only the enlarged movie render target is ever rescaled: game coordinates there are 512x512 */
    if (b && g_mr_res > 512 && (b[1] > 520.0f || b[3] > 520.0f)) return 0; /* already laid out for the enlarged target (the subtitle text is: its vertices run up to x 1400 / y 1600 at MovieRes 2048): draw as is, scaling them by 2/512 again put them far outside the picture */
        if (!rhw_supported(fvf)) { static int nu; if (nu++ < 30) logmsg("MRT unsupported pre-transformed FVF 0x%lx drawn UNSCALED", (unsigned long)fvf); return 0; } if (V.dev != dev) { V.ndecl = 0; V.vs_c = V.vs_n = NULL; V.dev = dev; }
        pvs = (fvf & D3DFVF_DIFFUSE) ? &V.vs_c : &V.vs_n; if (!*pvs && FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)((fvf & D3DFVF_DIFFUSE) ? g_vs_color : g_vs_nocolor), pvs))) return 0;
        if (!(d = v_decl(dev, fvf))) return 0; k[0] = k[1] = 2.0f / 512.0f; k[2] = k[3] = 0; if (b && fabsf(b[2] - 112.0f) < 3.0f && fabsf(b[3] - 400.0f) < 3.0f) g_mrt_movie = 1; g_mrt_moviepass = g_mrt_passno; if (b && g_mrt_npd < 48) { g_mrt_pd[g_mrt_npd][0] = b[0]; g_mrt_pd[g_mrt_npd][1] = b[1]; g_mrt_pd[g_mrt_npd][2] = b[2]; g_mrt_pd[g_mrt_npd][3] = b[3]; g_mrt_pd[g_mrt_npd][4] = (float)nv; g_mrt_npd++; } { static int nl2; if (cfg_dbg == 6 && nl2++ < 60) logmsg("MRT prep nv %u bbox %s%.0f,%.0f,%.0f,%.0f tex %u", nv, b ? "" : "(none) ", b ? b[0] : 0.0f, b ? b[1] : 0.0f, b ? b[2] : 0.0f, b ? b[3] : 0.0f, (unsigned)tex0_width(dev)); } 
    g_in_post = 1;
    IDirect3DDevice9_SetVertexDeclaration(dev, d);
    IDirect3DDevice9_SetVertexShader(dev, *pvs);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, k, 1);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 1, a, 1);
    return 1;
}
/* bounds of a draw's vertices (first float4 = x,y,z,rhw) */
static int bounds_mem(const void *p, UINT stride, UINT nv, float *b)
{
    UINT i; if (!p || stride < 16 || !nv) return 0; if (nv > 4096) nv = 4096;
    b[0] = b[2] = 1e9f; b[1] = b[3] = -1e9f;
    for (i = 0; i < nv; i++) { const float *f = (const float *)((const char *)p + (size_t)i * stride); if (f[0] < b[0]) b[0] = f[0]; if (f[0] > b[1]) b[1] = f[0]; if (f[1] < b[2]) b[2] = f[1]; if (f[1] > b[3]) b[3] = f[1]; }
    return 1;
}
static int bounds_vb(IDirect3DDevice9 *dev, INT base, UINT minv, UINT numv, float *b)
{
    IDirect3DVertexBuffer9 *vb = NULL; UINT off = 0, stride = 0; void *p = NULL; int ok = 0;
    if (FAILED(IDirect3DDevice9_GetStreamSource(dev, 0, &vb, &off, &stride)) || !vb) return 0;
    if (numv && numv <= 4096 && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, off + (UINT)(base + (INT)minv) * stride, numv * stride, &p, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && p) { /* NOOVERWRITE: do not wait for the GPU, we only read data the game already wrote */
        ok = bounds_mem(p, stride, numv, b); IDirect3DVertexBuffer9_Unlock(vb);
    }
    IDirect3DVertexBuffer9_Release(vb); return ok;
}
static void v_finish(IDirect3DDevice9 *dev, DWORD fvf)
{
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, fvf); /* also restores the game's own vertex format */
    g_in_post = 0; V.scaled++;
}
#define V_ACTIVE(fvf) (((V.on || (V.valid && ui_enabled())) && V.rt_bb && !g_in_post && rhw_fvf(fvf)) || (g_mrt && !g_in_post && rhw_fvf(fvf))) /* never rescale our own post-process quad */
static void virtual_adjust_pp(D3DPRESENT_PARAMETERS *pp)
{
    if (pp) { V.valid = 1; V.rt_bb = 1; V.vw = V.pw = pp->BackBufferWidth; V.vh = V.ph = pp->BackBufferHeight; }
    if (!cfg_vw || !cfg_vh || !pp) { V.on = 0; return; }
    if (pp->BackBufferWidth == (UINT)cfg_vw && pp->BackBufferHeight == (UINT)cfg_vh) {
        UINT pw = cfg_pw ? (UINT)cfg_pw : (UINT)GetSystemMetrics(SM_CXSCREEN), ph = cfg_ph ? (UINT)cfg_ph : (UINT)GetSystemMetrics(SM_CYSCREEN);
        V.on = 1; V.rt_bb = 1; V.vw = cfg_vw; V.vh = cfg_vh; V.pw = pw; V.ph = ph;
        pp->BackBufferWidth = pw; pp->BackBufferHeight = ph;
        logmsg("virtual resolution ON: game %ux%u -> back buffer %ux%u (scale %.3f x %.3f)", V.vw, V.vh, pw, ph, (double)pw / V.vw, (double)ph / V.vh);
    } else {
        V.on = 0;
        logmsg("virtual resolution off: game asked for %ux%u, configured virtual size is %dx%d", pp->BackBufferWidth, pp->BackBufferHeight, cfg_vw, cfg_vh);
    }
}
/* The fullscreen switch resizes the window to the real size; a game could learn its "screen size" again from WM_SIZE sent straight to its window procedure
 * (not via PeekMessage). Subclass the window and report the virtual size instead. (Observed: this game receives no WM_SIZE, kept as a safety net.) */
static WNDPROC g_orig_wp; static HWND g_sub_hwnd; static unsigned g_wm_log;
static LRESULT CALLBACK sub_wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (V.on && m == WM_SIZE && w != SIZE_MINIMIZED && LOWORD(l) == V.pw && HIWORD(l) == V.ph) { if (g_wm_log++ < 10) logmsg("WM_SIZE %ux%u rewritten to %ux%u", V.pw, V.ph, V.vw, V.vh); l = MAKELPARAM(V.vw, V.vh); }
    else if (V.on && m == WM_DISPLAYCHANGE && LOWORD(l) == V.pw && HIWORD(l) == V.ph) l = MAKELPARAM(V.vw, V.vh);
    return CallWindowProcA(g_orig_wp, h, m, w, l);
}
static void subclass_window(HWND h)
{
    if (!h || g_sub_hwnd == h) return;
    g_orig_wp = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)sub_wp); g_sub_hwnd = h;
    logmsg("window %p subclassed", (void *)h);
}

/* --- user-input API hooks (patched into the game exe's import table) --- */
static BOOL (WINAPI *r_S2C)(HWND, LPPOINT); static BOOL (WINAPI *r_C2S)(HWND, LPPOINT); static BOOL (WINAPI *r_GCR)(HWND, LPRECT);
static BOOL (WINAPI *r_Peek)(LPMSG, HWND, UINT, UINT, UINT); static BOOL (WINAPI *r_GCP)(LPPOINT);
static unsigned g_ncp, g_ns2c, g_npeek, g_nmv, g_nbt;
static int real_client(HWND h, int *w, int *hgt)
{
    RECT rc; if (!V.on || !G.hwnd || h != G.hwnd || !r_GCR || !r_GCR(h, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0;
    *w = rc.right; *hgt = rc.bottom; return 1;
}
/* The game reads the mouse as RELATIVE motion: WM_MOUSEMOVE carries (centre + delta) and the game warps the pointer back to the centre, integrating its own
 * cursor position P (drawn as a sprite). The HUD block / timeline strip are drawn zoomed but the game's hit areas are not, so we keep P = g(V), where V is
 * the position the user sees (integral of the real deltas) and g divides by the zoom inside the zoomed block. We feed the game the delta g(V_new)-g(V_old). */
static float g_vx, g_vy, g_px, g_py; static int g_vinit; static unsigned g_rlog;
static void hud_g(float vx, float vy, float *px, float *py)
{
    float sx = V.vw / 2560.0f, sy = V.vh / 1440.0f; *px = vx; *py = vy;
    if (cfg_hud != 1.0f && vx < HUD_RX * sx * cfg_hud && vy < HUD_RY * sy * cfg_hud) { *px = vx / cfg_hud + cfg_mx; *py = vy / cfg_hud + cfg_my; }
    else if (cfg_bar != 1.0f && vy < BAR_Y * sy * cfg_bar) *py = vy / cfg_bar;
}
/* The game's integrated cursor position P (hotspot; the sprite quad is drawn at P-5) lives in a few static float pairs in the exe (found with the cursor finder).
 * Before the game consumes a WM_MOUSEMOVE we write P := g(V_new) - delta; the game then adds its raw delta and lands exactly on g(V_new) (deltas stay raw, so camera drags keep working). */
static float *const g_pxy[][2] = { { (float *)0x0104CCE0, (float *)0x0104CCE4 }, { (float *)0x0104CD28, (float *)0x0104CD2C }, { (float *)0x0104E724, (float *)0x0104E728 }, { (float *)0x0105C430, (float *)0x0105C434 }, { (float *)0x0105CBD8, (float *)0x0105CBDC } };
static int g_pxok = -1;
static void hud_inv(float px, float py, float *vx, float *vy)
{
    float sx = V.vw / 2560.0f, sy = V.vh / 1440.0f; *vx = px; *vy = py;
    if (cfg_hud != 1.0f && px - cfg_mx < HUD_RX * sx && py - cfg_my < HUD_RY * sy) { *vx = (px - cfg_mx) * cfg_hud; *vy = (py - cfg_my) * cfg_hud; }
    else if (cfg_bar != 1.0f && py < BAR_Y * sy) *vy = py * cfg_bar;
}
static void hud_mouse_delta(HWND h, LPARAM *lp)
{
    int w, ht, x = (short)LOWORD(*lp), y = (short)HIWORD(*lp), cx, cy, dx, dy, i; float nx, ny, npx, npy, ivx, ivy; static unsigned nlog;
    if (cfg_hud == 1.0f || !V.valid || !G.hwnd || h != G.hwnd) { g_vinit = 0; return; }
    { RECT rc; if (!r_GCR || !r_GCR(h, &rc) || rc.right <= 0 || rc.bottom <= 0) return; w = rc.right; ht = rc.bottom; cx = w / 2; cy = ht / 2; }
    dx = x - cx; dy = y - cy; if (!dx && !dy) return; /* the warp back to the centre */
    if (abs(dx) > w / 4 || abs(dy) > ht / 4) return;
    if (g_pxok < 1) { /* sanity check: the static pair must hold the cursor (sprite quad top-left = P-5); retried until a cursor has been drawn, never latched off for good */
        float px = *g_pxy[0][0], py = *g_pxy[0][1]; static unsigned nchk;
        if (!g_cur_ok || (g_cur_x == 0 && g_cur_y == 0)) return;
        g_pxok = (fabsf(px - g_cur_x) < 24 && fabsf(py - g_cur_y) < 24) ? 1 : 0;
        if (nchk++ < 5 || g_pxok) logmsg("pointer variable check: P=(%.0f,%.0f) quad=(%.0f,%.0f) -> %s", px, py, g_cur_x, g_cur_y, g_pxok ? "ok" : "not matching (will retry)");
    }
    if (g_pxok != 1) return;
    hud_inv(*g_pxy[0][0], *g_pxy[0][1], &ivx, &ivy); /* where the game currently has the pointer, as the user sees it */
    if (!g_vinit || fabsf(ivx - g_vx) > 8 || fabsf(ivy - g_vy) > 8) { g_vx = ivx; g_vy = ivy; g_vinit = 1; }
    nx = g_vx + dx; ny = g_vy + dy; if (nx < 0) nx = 0; if (ny < 0) ny = 0; if (nx > V.vw - 1) nx = (float)V.vw - 1; if (ny > V.vh - 1) ny = (float)V.vh - 1;
    hud_g(nx, ny, &npx, &npy);
    { /* the game integrates in the float accumulators at 0x104ccf0/4 (pos += delta*sens, clamped, then rounded into the pair we read above): pre-bias them so the result lands on g(V) */
      float sens = *(volatile float *)0x00E52CA8; if (!(sens > 0.01f && sens < 100.0f)) sens = 1.0f;
      *(volatile float *)0x0104CCF0 = npx - dx * sens; *(volatile float *)0x0104CCF4 = npy - dy * sens; }
    if (((nx < 420 && ny < 120) && g_rlog++ < 3000) || nlog++ % 60 == 0) logmsg("hud_mouse: V (%.0f,%.0f) -> P (%.1f,%.1f) delta (%d,%d) quad (%.0f,%.0f)", nx, ny, npx, npy, dx, dy, g_cur_x, g_cur_y);
    g_vx = nx; g_vy = ny; (void)lp;
}
static BOOL WINAPI hk_GetCursorPos(LPPOINT p)
{
    BOOL r = r_GCP(p); if (r && p && (g_ncp++ < 3)) logmsg("GetCursorPos -> (%ld,%ld)", p->x, p->y); return r;
}
static BOOL WINAPI hk_ScreenToClient(HWND h, LPPOINT p)
{
    if (p && g_ns2c++ < 3) logmsg("ScreenToClient(%p) in (%ld,%ld)", (void *)h, p->x, p->y);
    int w, ht; BOOL r = r_S2C(h, p);
    if (r && p && real_client(h, &w, &ht)) { p->x = (LONG)((long long)p->x * V.vw / w); p->y = (LONG)((long long)p->y * V.vh / ht); V.calls_s2c++; }
    return r;
}
static BOOL WINAPI hk_ClientToScreen(HWND h, LPPOINT p)
{
    int w, ht;
    if (p && real_client(h, &w, &ht)) { p->x = (LONG)((long long)p->x * w / V.vw); p->y = (LONG)((long long)p->y * ht / V.vh); V.calls_c2s++; }
    return r_C2S(h, p);
}
static BOOL WINAPI hk_GetClientRect(HWND h, LPRECT rc)
{
    int w, ht; BOOL r = r_GCR(h, rc);
    if (r && rc && real_client(h, &w, &ht)) { rc->right = rc->left + V.vw; rc->bottom = rc->top + V.vh; }
    return r;
}
/* Money pill hit area: the game's hit test (widget layout) knows nothing about the zoomed money pill, and the rating's rect overlaps it. While the pointer is over the VISIBLE pill, the game's
 * pointer variables are moved to the equivalent spot of the pill's ORIGINAL finance-only rectangle (as seen by the game: hover, tooltip, click); the cursor sprite is drawn back at the real spot. */
static int g_pm_mask;
static void pm_restore(void)
{
    if (g_pm_on) { int i; for (i = 0; i < 5; i++) if (g_pm_mask & (1 << i)) { /* per axis: the game refreshes X and Y from separate events, so one axis may still hold our mapped value while the other is already real */
        volatile float *px = (volatile float *)g_pxy[i][0], *py = (volatile float *)g_pxy[i][1];
        if (fabsf(*px - g_pm_map[0]) < 0.6f) *px = g_pm_real[0]; if (fabsf(*py - g_pm_map[1]) < 0.6f) *py = g_pm_real[1]; } g_pm_on = 0; g_pm_mask = 0; }
}
static BOOL WINAPI hk_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT f)
{
    BOOL r; int w, ht;
    static int ntr; float p0 = *(volatile float *)0x0104CCE0, p1;
    pm_restore(); p1 = *(volatile float *)0x0104CCE0;
    r = r_Peek(m, h, a, b, f);
    { float p2 = *(volatile float *)0x0104CCE0; if (cfg_dbg == 5 && ntr < 600 && (p0 > 2000 || p2 > 2000) && (g_pm_on || p0 != p1)) { ntr++; logmsg("PEEK frame %lu r %d msg 0x%x acc %.1f P: in %.1f restored %.1f after %.1f mapped %.1f on %d map %.1f real %.1f", G.frame, (int)r, (r && m) ? m->message : 0, *(volatile float *)0x0104CCF0, p0, p1, p2, *(volatile float *)0x0104CCE0, g_pm_on, g_pm_map[0], g_pm_real[0]); } }
    if (r && m && m->message == WM_MOUSEMOVE) { int px = (short)LOWORD(m->lParam), py = (short)HIWORD(m->lParam); g_npeek++; if ((px != 1280 || py != 720) && g_nmv++ < 80) logmsg("Peek MOUSEMOVE raw (%d,%d)", px, py); }
    else if (r && m && m->message >= WM_LBUTTONDOWN && m->message <= WM_MBUTTONDBLCLK && g_nbt++ < 20) logmsg("Peek button msg 0x%x (%d,%d)", m->message, (short)LOWORD(m->lParam), (short)HIWORD(m->lParam));
    if (r && m && m->message >= WM_MOUSEMOVE && m->message <= WM_MBUTTONDBLCLK) {
        int x = (short)LOWORD(m->lParam), y = (short)HIWORD(m->lParam);
        if (real_client(m->hwnd, &w, &ht)) { x = (int)((long long)x * V.vw / w); y = (int)((long long)y * V.vh / ht); V.msgs++; }
        m->lParam = MAKELPARAM((WORD)x, (WORD)y); if (m->message == WM_MOUSEMOVE && (f & PM_REMOVE)) hud_mouse_delta(m->hwnd, &m->lParam);
    }
    return r;
}
static void patch_iat_mod(HMODULE exe, const char *modname, const char *fn, void *hook, void **orig)
{
    /* Match by resolved address, not by name: some old linkers leave OriginalFirstThunk empty, so the only thing in the IAT is the resolved pointer. */
    BYTE *b = (BYTE *)exe; IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)b; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + dh->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; IMAGE_IMPORT_DESCRIPTOR *id;
    HMODULE u32 = GetModuleHandleA(modname); void *real = u32 ? (void *)GetProcAddress(u32, fn) : NULL;
    if (!dd->VirtualAddress || !real) { logmsg("IAT: cannot hook %s (no import dir or export)", fn); return; }
    for (id = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress); id->Name; id++) {
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(b + id->FirstThunk);
        for (; ft->u1.Function; ft++) {
            DWORD prot;
            if ((void *)ft->u1.Function != real) continue;
            VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &prot);
            if (orig) *orig = (void *)ft->u1.Function;
            ft->u1.Function = (DWORD_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void *), prot, &prot);
            logmsg("IAT hooked %s", fn);
            return;
        }
    }
    logmsg("IAT: %s not found in the exe's imports", fn);
}
static void patch_iat(HMODULE exe, const char *fn, void *hook, void **orig) { patch_iat_mod(exe, "user32.dll", fn, hook, orig); }
static void virtual_install_input_hooks(void)
{
    static int done; HMODULE exe = GetModuleHandleA(NULL);
    if (done) return; done = 1;
    patch_iat(exe, "ScreenToClient", hk_ScreenToClient, (void **)&r_S2C);
    patch_iat(exe, "ClientToScreen", hk_ClientToScreen, (void **)&r_C2S);
    patch_iat(exe, "GetClientRect", hk_GetClientRect, (void **)&r_GCR);
    patch_iat(exe, "PeekMessageA", hk_PeekMessageA, (void **)&r_Peek);
    patch_iat(exe, "GetCursorPos", hk_GetCursorPos, (void **)&r_GCP);
    if (!r_S2C || !r_C2S || !r_GCR || !r_Peek) { logmsg("virtual: some input hooks missing (S2C=%p C2S=%p GCR=%p Peek=%p)", r_S2C, r_C2S, r_GCR, r_Peek); }
}


/* ---------------- size queries: the game must only ever see the virtual size ---------------- */
typedef HRESULT (WINAPI *PF_SGD)(IDirect3DSurface9 *, D3DSURFACE_DESC *);
typedef HRESULT (WINAPI *PF_GDM)(IDirect3DDevice9 *, UINT, D3DDISPLAYMODE *);
typedef HRESULT (WINAPI *PF_AGDM)(IDirect3D9 *, UINT, D3DDISPLAYMODE *);
typedef HRESULT (WINAPI *PF_SCDM)(IDirect3DSwapChain9 *, D3DDISPLAYMODE *);
typedef HRESULT (WINAPI *PF_SCPP)(IDirect3DSwapChain9 *, D3DPRESENT_PARAMETERS *);
static PF_SGD o_SGD; static PF_GDM o_GDM; static PF_AGDM o_AGDM; static PF_SCDM o_SCDM; static PF_SCPP o_SCPP;
static unsigned long n_sgd, n_gdm, n_agdm, n_scdm, n_scpp;
static HRESULT real_desc(IDirect3DSurface9 *s, D3DSURFACE_DESC *d) { return o_SGD ? o_SGD(s, d) : IDirect3DSurface9_GetDesc(s, d); }
static void virt_wh(UINT *w, UINT *h) { if (V.on && *w == V.pw && *h == V.ph) { *w = V.vw; *h = V.vh; } }
static HRESULT WINAPI h_SGD(IDirect3DSurface9 *s, D3DSURFACE_DESC *d)
{
    HRESULT hr = o_SGD(s, d);
    if (SUCCEEDED(hr) && d && cfg_mrts > 1.0f && !g_in_post && d->Width == g_mrt_px && d->Height == g_mrt_px && (d->Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) { d->Width = d->Height = 512; return hr; }
    if (SUCCEEDED(hr) && d && !g_in_post && V.on && d->Width == V.pw && d->Height == V.ph) { if (!n_sgd++) logmsg("game queried a surface size (virtualized)"); virt_wh(&d->Width, &d->Height); }
    return hr;
}
typedef HRESULT (WINAPI *PF_SLR)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (WINAPI *PF_SUR)(IDirect3DSurface9 *);
static PF_SLR o_SLR; static PF_SUR o_SUR;
static HRESULT WINAPI h_SLR(IDirect3DSurface9 *sf, D3DLOCKED_RECT *lr, const RECT *r, DWORD fl)
{
    D3DSURFACE_DESC dd; HRESULT hr = o_SLR(sf, lr, r, fl); static int nl, nlf; if (FAILED(hr) && nlf++ < 20) logmsg("Surface LockRect FAILED %lx", (unsigned long)hr);
    if (cfg_dbg == 6 && nl < 60 && SUCCEEDED(real_desc(sf, &dd)) && dd.Width >= 512 && dd.Width == dd.Height && dd.Width != 2560) { nl++; logmsg("LockRect %p %ux%u usage %lx flags %lx rect %ld,%ld,%ld,%ld -> %s pitch %d", (void *)sf, dd.Width, dd.Height, (unsigned long)dd.Usage, (unsigned long)fl, r ? r->left : -1, r ? r->top : -1, r ? r->right : -1, r ? r->bottom : -1, SUCCEEDED(hr) ? "ok" : "FAIL", SUCCEEDED(hr) && lr ? lr->Pitch : 0); }
    return hr;
}
static HRESULT WINAPI h_GDM(IDirect3DDevice9 *dv, UINT i, D3DDISPLAYMODE *m) { HRESULT hr = o_GDM(dv, i, m); if (SUCCEEDED(hr) && m && !g_in_post && V.on && m->Width == V.pw && m->Height == V.ph) { if (!n_gdm++) logmsg("game called Device::GetDisplayMode (virtualized)"); virt_wh(&m->Width, &m->Height); } return hr; }
static HRESULT WINAPI h_AGDM(IDirect3D9 *d3, UINT a, D3DDISPLAYMODE *m) { HRESULT hr = o_AGDM(d3, a, m); if (SUCCEEDED(hr) && m && V.on && m->Width == V.pw && m->Height == V.ph) { if (!n_agdm++) logmsg("game called GetAdapterDisplayMode (virtualized)"); virt_wh(&m->Width, &m->Height); } return hr; }
static HRESULT WINAPI h_SCDM(IDirect3DSwapChain9 *sc, D3DDISPLAYMODE *m) { HRESULT hr = o_SCDM(sc, m); if (SUCCEEDED(hr) && m && V.on && m->Width == V.pw && m->Height == V.ph) { if (!n_scdm++) logmsg("game called SwapChain::GetDisplayMode (virtualized)"); virt_wh(&m->Width, &m->Height); } return hr; }
static HRESULT WINAPI h_SCPP(IDirect3DSwapChain9 *sc, D3DPRESENT_PARAMETERS *p) { HRESULT hr = o_SCPP(sc, p); if (SUCCEEDED(hr) && p && V.on && p->BackBufferWidth == V.pw && p->BackBufferHeight == V.ph) { if (!n_scpp++) logmsg("game called SwapChain::GetPresentParameters (virtualized)"); virt_wh(&p->BackBufferWidth, &p->BackBufferHeight); } return hr; }
static void install_size_hooks(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL; IDirect3DSwapChain9 *sc = NULL; void *o;
    if ((o = patch_vt(dev, 8, h_GDM))) o_GDM = (PF_GDM)o;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { if ((o = patch_vt(bb, 12, h_SGD))) o_SGD = (PF_SGD)o; if ((o = patch_vt(bb, 13, h_SLR))) o_SLR = (PF_SLR)o; IDirect3DSurface9_Release(bb); }
    if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) && sc) { if ((o = patch_vt(sc, 7, h_SCDM))) o_SCDM = (PF_SCDM)o; if ((o = patch_vt(sc, 9, h_SCPP))) o_SCPP = (PF_SCPP)o; IDirect3DSwapChain9_Release(sc); }
}

/* ---------------- probe: logs one frame of the game's draw calls (Ctrl+Shift+F11 or F12, or create TheMoviesGfx.probe.now) ---------------- */
static FILE *g_pf; static int g_probe_left; static unsigned g_pn;
#define PROBING() (g_pf && !g_in_post)
static void plog(const char *fmt, ...) { va_list ap; if (!g_pf) return; va_start(ap, fmt); vfprintf(g_pf, fmt, ap); va_end(ap); fputc('\n', g_pf); }
typedef HRESULT (WINAPI *PF_DP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (WINAPI *PF_DIP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (WINAPI *PF_DPUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (WINAPI *PF_DIPUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
typedef HRESULT (WINAPI *PF_VP)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
typedef HRESULT (WINAPI *PF_SC)(IDirect3DDevice9 *, const RECT *);
typedef HRESULT (WINAPI *PF_RT)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (WINAPI *PF_TR)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
typedef HRESULT (WINAPI *PF_CLR)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT (WINAPI *PF_FVF)(IDirect3DDevice9 *, DWORD);
typedef HRESULT (WINAPI *PF_VD)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
typedef HRESULT (WINAPI *PF_VS)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
typedef HRESULT (WINAPI *PF_SW)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **);
typedef HRESULT (WINAPI *PF_SC0)(IDirect3DDevice9 *);
static PF_DP o_DP; static PF_DIP o_DIP; static PF_DPUP o_DPUP; static PF_DIPUP o_DIPUP; static PF_VP o_VP; static PF_SC o_SC; static PF_RT o_RT;
static PF_TR o_TR; static PF_CLR o_CLR; static PF_FVF o_FVF; static PF_VD o_VD; static PF_VS o_VS; static PF_SW o_SW; static PF_SC0 o_BEG, o_END;

static const char *usage_name(int u)
{
    static const char *n[] = {"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL","PSIZE","TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG","DEPTH","SAMPLE"};
    return u >= 0 && u < 14 ? n[u] : "?";
}
static void describe_state(IDirect3DDevice9 *dev, char *out, size_t cap)
{
    char decl[200] = "", rt[40] = "?", tex[40] = "-", vp[64] = "?"; DWORD fvf = 0, z = 0, ab = 0, cull = 0; int vs = 0, ps = 0;
    IDirect3DVertexDeclaration9 *vd = NULL; IDirect3DSurface9 *s = NULL; IDirect3DBaseTexture9 *bt = NULL; D3DVIEWPORT9 v;
    IDirect3DVertexShader9 *pvs = NULL; IDirect3DPixelShader9 *pps = NULL;
    IDirect3DDevice9_GetFVF(dev, &fvf);
    if (SUCCEEDED(IDirect3DDevice9_GetVertexDeclaration(dev, &vd)) && vd) {
        UINT n = 0; D3DVERTEXELEMENT9 el[32];
        if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration(vd, el, &n))) {
            UINT i; for (i = 0; i < n && i < 16 && el[i].Stream != 0xFF; i++) {
                char t[40]; sprintf(t, "%s%d@%d%s", usage_name(el[i].Usage), el[i].UsageIndex, el[i].Offset, el[i].Stream ? "s1" : "");
                if (strlen(decl) + strlen(t) + 2 < sizeof decl) { strcat(decl, t); strcat(decl, " "); }
            }
        }
        IDirect3DVertexDeclaration9_Release(vd);
    }
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &s)) && s) { D3DSURFACE_DESC d; real_desc(s, &d); sprintf(rt, "%ux%u", d.Width, d.Height); IDirect3DSurface9_Release(s); }
    if (SUCCEEDED(IDirect3DDevice9_GetTexture(dev, 0, &bt)) && bt) {
        if (IDirect3DBaseTexture9_GetType(bt) == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC d; IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)bt, 0, &d); sprintf(tex, "%ux%u%s", d.Width, d.Height, (d.Usage & D3DUSAGE_RENDERTARGET) ? "RT" : ""); }
        else strcpy(tex, "nontex2d");
        IDirect3DBaseTexture9_Release(bt);
    }
    if (SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &v))) sprintf(vp, "%lu,%lu,%lux%lu", v.X, v.Y, v.Width, v.Height);
    if (SUCCEEDED(IDirect3DDevice9_GetVertexShader(dev, &pvs)) && pvs) { vs = 1; IDirect3DVertexShader9_Release(pvs); }
    if (SUCCEEDED(IDirect3DDevice9_GetPixelShader(dev, &pps)) && pps) { ps = 1; IDirect3DPixelShader9_Release(pps); }
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &z); IDirect3DDevice9_GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &ab); IDirect3DDevice9_GetRenderState(dev, D3DRS_CULLMODE, &cull);
    snprintf(out, cap, "fvf=0x%lx%s decl=[%s] vs=%d ps=%d rt=%s vp=%s tex0=%s z=%lu ab=%lu", fvf, (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? "(RHW)" : "", decl, vs, ps, rt, vp, tex, z, ab);
}
static void log_draw(IDirect3DDevice9 *dev, const char *kind, int prim, UINT cnt, const void *vdata, UINT stride)
{
    char st[600], bb[96] = "";
    if (g_pn++ > 6000) return;
    describe_state(dev, st, sizeof st);
    if (vdata && stride >= 16) { /* user-pointer draw: bounding box of the first float4 (x,y,z,rhw) of the vertices we can see */
        UINT nv = cnt * (prim == D3DPT_TRIANGLELIST ? 3 : prim == D3DPT_TRIANGLESTRIP || prim == D3DPT_TRIANGLEFAN ? 1 : 2) + (prim == D3DPT_TRIANGLESTRIP ? 2 : 0), i;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        if (nv > 64) nv = 64;
        for (i = 0; i < nv; i++) { const float *p = (const float *)((const char *)vdata + (size_t)i * stride); if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1]; }
        sprintf(bb, " bbox x%.1f..%.1f y%.1f..%.1f stride=%u", x0, x1, y0, y1, stride);
    }
    plog("#%u %s prim=%d cnt=%u %s%s", g_pn, kind, prim, cnt, st, bb);
}

static void vb_bbox(IDirect3DDevice9 *dev, INT base, UINT minv, UINT numv, char *out, size_t cap)
{
    IDirect3DVertexBuffer9 *vb = NULL; UINT off = 0, stride = 0; void *p = NULL; out[0] = 0;
    if (FAILED(IDirect3DDevice9_GetStreamSource(dev, 0, &vb, &off, &stride)) || !vb || stride < 16) { if (vb) IDirect3DVertexBuffer9_Release(vb); return; }
    if (numv > 128) numv = 128;
    if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, off + (UINT)(base + (INT)minv) * stride, numv * stride, &p, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && p) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f; UINT i, uo = 16 + ((V.fvf & D3DFVF_DIFFUSE) ? 4 : 0); int hasuv = (V.fvf & D3DFVF_TEXCOUNT_MASK) && stride >= uo + 8;
        for (i = 0; i < numv; i++) {
            const float *f = (const float *)((const char *)p + (size_t)i * stride), *t = (const float *)((const char *)p + (size_t)i * stride + uo);
            if (f[0] < x0) x0 = f[0]; if (f[0] > x1) x1 = f[0]; if (f[1] < y0) y0 = f[1]; if (f[1] > y1) y1 = f[1];
            if (hasuv) { if (t[0] < u0) u0 = t[0]; if (t[0] > u1) u1 = t[0]; if (t[1] < v0) v0 = t[1]; if (t[1] > v1) v1 = t[1]; }
        }
        if (numv == 4 && stride == 28 && getf("ProbeVerts", 0.0f) > 1.5f && y0 > 1000.0f) { UINT k; plog("   COLORS:"); for (k = 0; k < 4; k++) plog("      %08lx", *(const unsigned long *)((const char *)p + (size_t)k * stride + 16)); }
        if (hasuv && numv >= 20 && numv <= 64 && x0 > 400.0f && y0 > 100.0f && getf("ProbeVerts", 0.0f) > 0.5f) { UINT k; plog("   VERTS nv=%u:", numv); for (k = 0; k < numv && k < 64; k++) { const float *f = (const float *)((const char *)p + (size_t)k * stride), *t = (const float *)((const char *)p + (size_t)k * stride + uo); plog("      %u x %.1f y %.1f u %.4f v %.4f", k, f[0], f[1], t[0], t[1]); } }
        if (hasuv) snprintf(out, cap, "   vbbox x%.1f..%.1f y%.1f..%.1f (nv=%u stride=%u) uv u%.4f..%.4f v%.4f..%.4f", x0, x1, y0, y1, numv, stride, u0, u1, v0, v1);
        else snprintf(out, cap, "   vbbox x%.1f..%.1f y%.1f..%.1f (nv=%u stride=%u)", x0, x1, y0, y1, numv, stride);
        IDirect3DVertexBuffer9_Unlock(vb);
    }
    IDirect3DVertexBuffer9_Release(vb);
}
static void dbg7(IDirect3DDevice9 *d, const char *k, UINT cnt)
{
    static int nl; static unsigned long lastf; IDirect3DBaseTexture9 *bt = NULL; D3DSURFACE_DESC dd; D3DVIEWPORT9 vp; DWORD fvf = 0;
    if (cfg_dbg != 7 || nl > 300 || G.frame - lastf < 25) return;
    if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, 0, &bt)) && bt) { if (IDirect3DBaseTexture9_GetType(bt) == D3DRTYPE_TEXTURE) { IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)bt, 0, &dd); if (dd.Width == 1024 && dd.Height == 1024) { IDirect3DDevice9_GetFVF(d, &fvf); IDirect3DDevice9_GetViewport(d, &vp); nl++; lastf = G.frame; logmsg("TEX1024 frame %lu %s cnt %u fvf 0x%lx vp %lu,%lu %lux%lu", G.frame, k, cnt, (unsigned long)fvf, vp.X, vp.Y, vp.Width, vp.Height); } } IDirect3DBaseTexture9_Release(bt); }
}
static int mrt_skip(void) { int i; if (!g_mrt) return 0; i = g_mrt_idx++; return i == cfg_mrtskip || (cfg_mrtstop >= 0 && i > cfg_mrtstop); }
static void mrt_states(IDirect3DDevice9 *d, DWORD fvf) { if (g_mrt && rhw_fvf(fvf)) { IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE); if (cfg_dbg == 6) IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE); if (getf("MrtNoBlend", 0.0f) > 0.5f) IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE); } }
static int hit_dump_ok_flag = 1;
static void mrt_state_log(IDirect3DDevice9 *d, INT b, UINT m, UINT n)
{
    if (!g_mrt || cfg_dbg != 6) return;
    int hit_dump_ok; { char t9[MAX_PATH]; strcpy(t9, g_logp); strcpy(strrchr(t9, (char)92) + 1, "TheMoviesGfx.rtdump.now"); hit_dump_ok = (GetFileAttributesA(t9) != INVALID_FILE_ATTRIBUTES); }
    static int nl; DWORD z, zw, st, ab, sb, db, sc, bo, cu, sf, da; char bb[200] = ""; D3DVIEWPORT9 vp;
    if (!g_mrt || cfg_dbg != 6) return;
    { IDirect3DVertexBuffer9 *vb0 = NULL; UINT off0 = 0, st0 = 0; void *pv0 = NULL; int hit = 0; if (n == 4 && SUCCEEDED(IDirect3DDevice9_GetStreamSource(d, 0, &vb0, &off0, &st0)) && vb0) { if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb0, off0 + (UINT)(b + (INT)m) * st0, 4 * st0, &pv0, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && pv0) { const float *f0 = (const float *)pv0, *f2 = (const float *)((const char *)pv0 + 2 * st0); hit = (fabsf(f0[1] - 112.0f) < 3.0f || fabsf(f2[1] - 400.0f) < 3.0f) && fabsf(f0[0] - 3.0f) < 3.0f; IDirect3DVertexBuffer9_Unlock(vb0); } IDirect3DVertexBuffer9_Release(vb0); } if (!hit && nl++ >= 0) return; }
    IDirect3DDevice9_GetRenderState(d, D3DRS_ZENABLE, &z); IDirect3DDevice9_GetRenderState(d, D3DRS_ZWRITEENABLE, &zw); IDirect3DDevice9_GetRenderState(d, D3DRS_STENCILENABLE, &st);
    IDirect3DDevice9_GetRenderState(d, D3DRS_ALPHABLENDENABLE, &ab); IDirect3DDevice9_GetRenderState(d, D3DRS_SRCBLEND, &sb); IDirect3DDevice9_GetRenderState(d, D3DRS_DESTBLEND, &db);
    IDirect3DDevice9_GetRenderState(d, D3DRS_SCISSORTESTENABLE, &sc); IDirect3DDevice9_GetRenderState(d, D3DRS_COLORWRITEENABLE, &bo); IDirect3DDevice9_GetRenderState(d, D3DRS_CULLMODE, &cu);
    IDirect3DDevice9_GetRenderState(d, D3DRS_ZFUNC, &sf); IDirect3DDevice9_GetRenderState(d, D3DRS_ALPHATESTENABLE, &da); IDirect3DDevice9_GetViewport(d, &vp);
    { IDirect3DVertexBuffer9 *vb = NULL; UINT off = 0, stride = 0; void *pv = NULL; if (SUCCEEDED(IDirect3DDevice9_GetStreamSource(d, 0, &vb, &off, &stride)) && vb) { if (n && n <= 8 && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, off + (UINT)(b + (INT)m) * stride, n * stride, &pv, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && pv) { UINT q; for (q = 0; q < n; q++) { const float *f = (const float *)((const char *)pv + q * stride); logmsg("MRTV %u: %.2f %.2f %.2f %.2f col %08lx uv %.3f %.3f", q, f[0], f[1], f[2], f[3], *(const unsigned long *)(f + 4), f[5], f[6]); } IDirect3DVertexBuffer9_Unlock(vb); } IDirect3DVertexBuffer9_Release(vb); } }
    g_in_post = 1; vb_bbox(d, b, m, n, bb, sizeof bb); g_in_post = 0;
    { IDirect3DBaseTexture9 *bt = NULL; char ts[120] = "none"; if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, 0, &bt)) && bt) { D3DSURFACE_DESC dd; IDirect3DTexture9 *t2 = (IDirect3DTexture9 *)bt; if (o_TGLD ? SUCCEEDED(o_TGLD(t2, 0, &dd)) : SUCCEEDED(IDirect3DTexture9_GetLevelDesc(t2, 0, &dd))) snprintf(ts, sizeof ts, "tex %p %ux%u fmt%d usage0x%lx pool%d lv%lu%s", (void *)bt, dd.Width, dd.Height, (int)dd.Format, (unsigned long)dd.Usage, (int)dd.Pool, (unsigned long)IDirect3DBaseTexture9_GetLevelCount(bt), (void *)bt == g_mrt_tex ? " (THE MOVIE RT)" : ""); IDirect3DBaseTexture9_Release(bt); } logmsg("MRTTEX %s", ts); }
    { IDirect3DBaseTexture9 *bt2 = NULL; if (hit_dump_ok && SUCCEEDED(IDirect3DDevice9_GetTexture(d, 0, &bt2)) && bt2) { IDirect3DTexture9 *t3 = (IDirect3DTexture9 *)bt2; D3DLOCKED_RECT lr2; D3DSURFACE_DESC d2; if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(t3, 0, &d2)) && SUCCEEDED(IDirect3DTexture9_LockRect(t3, 0, &lr2, NULL, D3DLOCK_READONLY))) { char tp[MAX_PATH]; FILE *tf; strcpy(tp, g_logp); strcpy(strrchr(tp, (char)92) + 1, "TheMoviesGfx.tex0.raw"); if ((tf = fopen(tp, "wb"))) { fwrite(&d2.Width, 4, 1, tf); fwrite(&d2.Height, 4, 1, tf); fwrite(&d2.Format, 4, 1, tf); fwrite(&lr2.Pitch, 4, 1, tf); fwrite(lr2.pBits, 1, (size_t)(d2.Width / 4) * (d2.Height / 4) * 16, tf); fclose(tf); logmsg("tex0 dumped %ux%u fmt %lx pitch %d", d2.Width, d2.Height, (unsigned long)d2.Format, lr2.Pitch); } IDirect3DTexture9_UnlockRect(t3, 0); hit_dump_ok = 0; } IDirect3DBaseTexture9_Release(bt2); } }
    { int st; char sl[240] = ""; for (st = 0; st < 8; st++) { IDirect3DBaseTexture9 *tt = NULL; if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, st, &tt)) && tt) { char one[40]; snprintf(one, sizeof one, "[%d]%p%s ", st, (void *)tt, (void *)tt == g_mrt_tex ? "=RT!" : ""); strcat(sl, one); IDirect3DBaseTexture9_Release(tt); } } logmsg("MRTSTAGES %s", sl); }
    { DWORD v[16]; int st; D3DSURFACE_DESC d1; IDirect3DBaseTexture9 *t1 = NULL; for (st = 0; st < 2; st++) { IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_COLOROP, &v[0]); IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_COLORARG1, &v[1]); IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_COLORARG2, &v[2]); IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_ALPHAOP, &v[3]); IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_ALPHAARG1, &v[4]); IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_ALPHAARG2, &v[5]); IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_TEXCOORDINDEX, &v[6]); IDirect3DDevice9_GetTextureStageState(d, st, D3DTSS_TEXTURETRANSFORMFLAGS, &v[7]); logmsg("MRTSTAGE%d cop %lu c1 %lx c2 %lx aop %lu a1 %lx a2 %lx tci %lx ttf %lu", st, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); }
      if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, 1, &t1)) && t1) { if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)t1, 0, &d1))) logmsg("MRTSTAGE1TEX %ux%u fmt %lx usage %lx pool %d", d1.Width, d1.Height, (unsigned long)d1.Format, (unsigned long)d1.Usage, (int)d1.Pool); IDirect3DBaseTexture9_Release(t1); } }
    logmsg("MRTST z%lu zw%lu zf%lu st%lu ab%lu sb%lu db%lu sc%lu cw%lx cull%lu at%lu | %s", z, zw, sf, st, ab, sb, db, sc, bo, cu, da, bb);
}
static void mrt_ib_log(IDirect3DDevice9 *d, INT b, UINT m, UINT n, UINT st, UINT c)
{
    static int nl; IDirect3DIndexBuffer9 *ib = NULL; void *p = NULL; D3DINDEXBUFFER_DESC id;
    if (!g_mrt || cfg_dbg != 6 || nl++ >= 12) return;
    if (SUCCEEDED(IDirect3DDevice9_GetIndices(d, &ib)) && ib) { IDirect3DIndexBuffer9_GetDesc(ib, &id); if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, st * 2, c * 3 * 2, &p, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && p) { unsigned short *ix = (unsigned short *)p; UINT q; char t[200] = ""; for (q = 0; q < c * 3 && q < 12; q++) { char o[16]; snprintf(o, sizeof o, "%u ", ix[q]); strcat(t, o); } logmsg("MRTIB base %ld minidx %u numv %u startidx %u prims %u fmt %d: %s", (long)b, m, n, st, c, (int)id.Format, t); IDirect3DIndexBuffer9_Unlock(ib); } IDirect3DIndexBuffer9_Release(ib); }
}
static void mrt_3d_log(IDirect3DDevice9 *d, INT b, UINT m, UINT n)
{
    static int nl; IDirect3DVertexBuffer9 *vb = NULL; UINT off = 0, st = 0; void *pv = NULL; D3DMATRIX w, v, pr; D3DVIEWPORT9 vp;
    if (!g_mrt || cfg_dbg != 6 || V.fvf != 0x142 || n != 4 || nl++ >= 6) return;
    if (SUCCEEDED(IDirect3DDevice9_GetStreamSource(d, 0, &vb, &off, &st)) && vb) { if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, off + (UINT)(b + (INT)m) * st, 4 * st, &pv, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && pv) { UINT q; for (q = 0; q < 4; q++) { const float *f = (const float *)((const char *)pv + q * st); logmsg("MRT3D v%u: %.2f %.2f %.2f col %08lx uv %.3f %.3f (stride %u)", q, f[0], f[1], f[2], *(const unsigned long *)(f + 3), f[4], f[5], st); } IDirect3DVertexBuffer9_Unlock(vb); } IDirect3DVertexBuffer9_Release(vb); }
    IDirect3DDevice9_GetTransform(d, D3DTS_WORLD, &w); IDirect3DDevice9_GetTransform(d, D3DTS_VIEW, &v); IDirect3DDevice9_GetTransform(d, D3DTS_PROJECTION, &pr); IDirect3DDevice9_GetViewport(d, &vp);
    logmsg("MRT3D world %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f t %.2f %.2f %.2f", w._11, w._12, w._13, w._21, w._22, w._23, w._31, w._32, w._33, w._41, w._42, w._43);
    logmsg("MRT3D view  %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f t %.2f %.2f %.2f", v._11, v._12, v._13, v._21, v._22, v._23, v._31, v._32, v._33, v._41, v._42, v._43);
    logmsg("MRT3D proj  %.4f %.4f %.4f %.4f | %.4f %.4f | vp %lu,%lu %lux%lu (real %lu)", pr._11, pr._22, pr._33, pr._34, pr._41, pr._42, vp.X, vp.Y, vp.Width, vp.Height, vp.Width);
}
static void mrt_seq(const char *w) { static int nl; if (g_mrt && cfg_dbg == 6 && nl++ < 150) logmsg("SEQ %s pass %d", w, g_mrt_passno); }
static void mrt_log(const char *w, DWORD fvf, UINT n) { if (g_mrt && g_mrt_nall < 64) { g_mrt_all[g_mrt_nall][0] = fvf; g_mrt_all[g_mrt_nall][1] = n; g_mrt_nall++; } static int nl; if (g_mrt && cfg_dbg == 6 && nl++ < 120) { D3DVIEWPORT9 vp; IDirect3DDevice9_GetViewport(g_mrt_dev, &vp); logmsg("MRT %s fvf 0x%lx n %u vp %lu,%lu %lux%lu", w, (unsigned long)fvf, n, vp.X, vp.Y, vp.Width, vp.Height); } }
static HRESULT WINAPI h_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT c) { HRESULT hr; g_mrt_dev = d; mrt_log("DP", V.fvf, c); dbg7(d, "DP", c); if (PROBING()) log_draw(d, "DP", t, c, NULL, 0); if (V_ACTIVE(V.fvf)) { float bb[4]; UINT nv = (t == D3DPT_TRIANGLELIST) ? c * 3 : (t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN) ? c + 2 : 0; int hb = nv && bounds_vb(d, 0, a, nv, bb); if (v_prepare(d, V.fvf, hb ? bb : NULL, nv)) { hr = o_DP(d, t, a, c); v_finish(d, V.fvf); return hr; } } return o_DP(d, t, a, c); }
/* Movie-Maker small-font text: every glyph quad of a run is a window of fixed height (the font's line box) into the glyph atlas, 4 texels short of the descenders (p, g, y, comma...). Extend the bottom edge of each quad by MakerDescent texels (y and v) before drawing. */
/* Movie-Maker dialog title ("Advanced Movie-Making"): its glyph runs are drawn BEFORE the "Make Your Script" band quad, which then covers the descenders (the g). The runs are held back and drawn right after the band (or at Present if the band never comes). */
#define MKDEF 32
#define g_in_post_defer() 0
static struct { unsigned char vb[256 * 24]; unsigned short ib[1536]; UINT n, c; float bb[4]; IDirect3DBaseTexture9 *tex; DWORD rs[6], ts[7]; } g_def[MKDEF]; static int g_ndef; static const D3DRENDERSTATETYPE mk_rs[6] = { D3DRS_TEXTUREFACTOR, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE, D3DRS_ZENABLE }; static const D3DTEXTURESTAGESTATETYPE mk_ts[7] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX };
/* fast path for mk_draw: our own dynamic vertex ring + a static quad index buffer (0,1,2,0,2,3 per quad, verified against the game's own index data for the first draws) instead of DrawIndexedPrimitiveUP + locking the game's index buffer (that cost ~12 ms per frame with the Sets dialog open) */
#define MKRING (1u << 20)
static IDirect3DVertexBuffer9 *g_mkvb; static IDirect3DIndexBuffer9 *g_mkib; static UINT g_mkoff; static int g_mkpat, g_mkchk;
static void mk_ring_release(void) { if (g_mkvb) { IDirect3DVertexBuffer9_Release(g_mkvb); g_mkvb = NULL; } if (g_mkib) { IDirect3DIndexBuffer9_Release(g_mkib); g_mkib = NULL; } g_mkoff = 0; }
static int mk_ring_make(IDirect3DDevice9 *d)
{
    void *p; UINT q;
    if (g_mkvb && g_mkib) return 1; mk_ring_release();
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(d, MKRING, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_mkvb, NULL)) || !g_mkvb) { g_mkvb = NULL; return 0; }
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(d, 64 * 6 * 2 * 4, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &g_mkib, NULL)) || !g_mkib) { mk_ring_release(); return 0; }
    if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(g_mkib, 0, 0, &p, 0)) && p) { unsigned short *ix = (unsigned short *)p; for (q = 0; q < 64 * 4; q++) { ix[q * 6] = (unsigned short)(q * 4); ix[q * 6 + 1] = (unsigned short)(q * 4 + 1); ix[q * 6 + 2] = (unsigned short)(q * 4 + 2); ix[q * 6 + 3] = (unsigned short)(q * 4); ix[q * 6 + 4] = (unsigned short)(q * 4 + 2); ix[q * 6 + 5] = (unsigned short)(q * 4 + 3); } IDirect3DIndexBuffer9_Unlock(g_mkib); } else { mk_ring_release(); return 0; }
    return 1;
}
/* per font atlas ink bitmap (1 byte per texel, alpha > 2/15) cached per texture pointer: a glyph quad is extended downwards only by the rows of its OWN ink below the cell (counted until the first blank row of its column range). With tight packing (main-menu font) there is no blank row below -> no extension, so no junk from the next glyph row shows. */
static struct { void *tex; UINT w, h; unsigned char *ink; int roomy; } g_atl[16]; static int g_natl;
static volatile int g_ink_reset;
static int mk_atlas(IDirect3DBaseTexture9 *bt)
{
    int i; D3DSURFACE_DESC dd; D3DLOCKED_RECT lr;
    if (g_ink_reset) { g_ink_reset = 0; for (i = 0; i < g_natl; i++) { free(g_atl[i].ink); g_atl[i].ink = NULL; } g_natl = 0; }
    for (i = 0; i < g_natl; i++) if (g_atl[i].tex == (void *)bt) return i;
    if (g_natl >= 16) return -1;
    if (IDirect3DBaseTexture9_GetType(bt) != D3DRTYPE_TEXTURE || FAILED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)bt, 0, &dd))) return -1;
    g_atl[g_natl].tex = (void *)bt; g_atl[g_natl].w = dd.Width; g_atl[g_natl].h = dd.Height; g_atl[g_natl].ink = NULL;
    if ((dd.Format == D3DFMT_A4R4G4B4 || dd.Format == D3DFMT_A8R8G8B8 || dd.Format == D3DFMT_A8L8 || dd.Format == D3DFMT_A8) && dd.Width <= 1024 && dd.Height <= 1024 && SUCCEEDED(IDirect3DTexture9_LockRect((IDirect3DTexture9 *)bt, 0, &lr, NULL, D3DLOCK_READONLY))) {
        unsigned char *ink = (unsigned char *)malloc((size_t)dd.Width * dd.Height); UINT y, x;
        if (ink) { for (y = 0; y < dd.Height; y++) { const unsigned char *row = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch; for (x = 0; x < dd.Width; x++) { unsigned a8; if (dd.Format == D3DFMT_A4R4G4B4) a8 = (((const unsigned short *)row)[x] >> 12) & 15; else if (dd.Format == D3DFMT_A8R8G8B8) a8 = row[x * 4 + 3] >> 4; else if (dd.Format == D3DFMT_A8L8) a8 = row[x * 2 + 1] >> 4; else a8 = row[x] >> 4; ink[(size_t)y * dd.Width + x] = a8 > 2; } } g_atl[g_natl].ink = ink; }
        IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)bt, 0);
        if (ink) { UINT y2, x2; int gap = 0, best = 0, seen = 0; for (y2 = 0; y2 < dd.Height; y2++) { int any = 0; for (x2 = 0; x2 < dd.Width && !any; x2++) any = ink[(size_t)y2 * dd.Width + x2]; if (any) { if (seen && gap > best) best = gap; seen = 1; gap = 0; } else if (seen) gap++; } g_atl[g_natl].roomy = best >= 8; /* only fonts with real blank bands between their glyph rows (standard font ~14-22 rows) can hide tails; tightly packed fonts (the main menu) are skipped entirely: processing their ~175 runs per frame cost 50 ms */ }
        logmsg("font atlas %p %ux%u fmt %d: ink bitmap cached (roomy=%d)", (void *)bt, dd.Width, dd.Height, (int)dd.Format, g_atl[g_natl].roomy);
        if (ink && cfg_textdebug) { char fn[MAX_PATH]; FILE *bf; UINT yy, xx; sprintf(fn, "%s", g_logp); strcpy(strrchr(fn, '\\') + 1, "TheMoviesGfx.ink"); sprintf(fn + strlen(fn), "%d.pgm", g_natl); if ((bf = fopen(fn, "wb"))) { fprintf(bf, "P5 %u %u 255\n", dd.Width, dd.Height); for (yy = 0; yy < dd.Height; yy++) for (xx = 0; xx < dd.Width; xx++) fputc(ink[(size_t)yy * dd.Width + xx] ? 255 : 0, bf); fclose(bf); } }
    }
    g_natl++; return g_natl - 1;
}
/* number of texel rows below texel row yb that carry ink inside columns x0..x1 before the first blank row (0 when none, or when no blank row is found within lim rows) */
static int mk_tail_rows(int ai, int x0, int x1, int yb, int lim)
{
    int r, x; const unsigned char *ink = g_atl[ai].ink; int W = (int)g_atl[ai].w, H = (int)g_atl[ai].h; if (!ink) return 0;
    x0++; x1--; /* ignore the outermost columns (outline overhang of the neighbouring cells) */ if (x0 < 0) x0 = 0; if (x1 >= W) x1 = W - 1;
    for (r = 0; r < lim; r++) { int y = yb + r, any = 0; if (y < 0 || y >= H) return 0; for (x = x0; x <= x1; x++) if (ink[(size_t)y * W + x] && ++any > 1) break; if (any <= 1) return r; }
    return 0;
}
/* glyph quads repeat endlessly (same atlas cell, same size), so the tail-row scan is cached: direct-mapped table keyed on (atlas, u0, u1, v, limit) */
static struct { unsigned long long key; int tr; } g_tc[1 << 14]; static unsigned long g_mkq, g_mkq_hit;
static int mk_tail_rows_c(int ai, int x0, int x1, int yb, int lim)
{
    unsigned long long k = 1ull + ((unsigned long long)(ai & 15) | ((unsigned long long)(x0 & 0x7ff) << 4) | ((unsigned long long)(x1 & 0x7ff) << 15) | ((unsigned long long)(yb & 0x7ff) << 26) | ((unsigned long long)(lim & 0x3f) << 37));
    unsigned h = (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 50); int tr; g_mkq++;
    if (g_tc[h].key == k) { g_mkq_hit++; return g_tc[h].tr; }
    tr = mk_tail_rows(ai, x0, x1, yb, lim); g_tc[h].key = k; g_tc[h].tr = tr; return tr;
}
/* width of the texture in stage 0, cached per texture pointer (refreshed every 300 frames) instead of GetLevelDesc on every draw */
static struct { void *tex; UINT w; } g_tw[64]; static int g_tw_next; static unsigned long g_tw_frame;
static UINT tex_w_cached(IDirect3DBaseTexture9 *bt)
{
    int i; UINT w = 0;
    if (G.frame - g_tw_frame >= 300) { memset(g_tw, 0, sizeof g_tw); g_tw_frame = G.frame; }
    for (i = 0; i < 64; i++) if (g_tw[i].tex == (void *)bt) return g_tw[i].w;
    if (IDirect3DBaseTexture9_GetType(bt) == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC dd; if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)bt, 0, &dd))) w = dd.Width; }
    g_tw[g_tw_next].tex = (void *)bt; g_tw[g_tw_next].w = w; g_tw_next = (g_tw_next + 1) & 63; return w;
}
static unsigned long g_mkp_calls, g_mkp_cand, g_mkp_mod, g_mkp_verts; static double g_mkp_ms;
static LONGLONG g_mkt[8], g_mk_t0; static unsigned long g_mk_proc;
#define MKT(i) do { LARGE_INTEGER _n; QueryPerformanceCounter(&_n); g_mkt[i] += _n.QuadPart - g_mk_t0; g_mk_t0 = _n.QuadPart; } while (0)
static HRESULT mk_draw_impl(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT n, UINT st, UINT c);
static HRESULT mk_draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT n, UINT st, UINT c)
{
    LARGE_INTEGER q0, q1, fq; HRESULT hr; QueryPerformanceCounter(&q0); hr = mk_draw_impl(d, t, b, m, n, st, c); QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
    g_mkp_calls++; g_mkp_verts += n; g_mkp_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart; return hr;
}
static HRESULT mk_draw_impl(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT n, UINT st, UINT c)
{
    IDirect3DVertexBuffer9 *vb = NULL; IDirect3DIndexBuffer9 *ib = NULL; UINT off = 0, stride = 0; D3DINDEXBUFFER_DESC idesc; void *pv = NULL, *pi = NULL; static unsigned char vbuf[256 * 24]; static unsigned short ibuf[1536]; HRESULT hr = D3D_OK; UINT q, k; int ok = 0, ai = -1;
    if (cfg_textdesc < 0.5f || cfg_mkdesc <= 0.0f || V.fvf != 0x104 || t != D3DPT_TRIANGLELIST || n < 4 || n > 256 || (n & 3) || c * 3 > 1536 || g_mrt) return o_DIP(d, t, b, m, n, st, c);
    { LARGE_INTEGER _s; QueryPerformanceCounter(&_s); g_mk_t0 = _s.QuadPart; g_mk_proc++; }
    { IDirect3DBaseTexture9 *tb = NULL; if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, 0, &tb)) && tb) { if (tex_w_cached(tb) == 256) ai = mk_atlas(tb); IDirect3DBaseTexture9_Release(tb); } if (ai < 0 || !g_atl[ai].ink || !g_atl[ai].roomy) return o_DIP(d, t, b, m, n, st, c); }
    MKT(0);
    if (FAILED(IDirect3DDevice9_GetStreamSource(d, 0, &vb, &off, &stride)) || !vb || stride != 24 || FAILED(IDirect3DDevice9_GetIndices(d, &ib)) || !ib || FAILED(IDirect3DIndexBuffer9_GetDesc(ib, &idesc)) || idesc.Format != D3DFMT_INDEX16) goto fallback;
    MKT(1);
    if (FAILED(IDirect3DVertexBuffer9_Lock(vb, off + (UINT)(b + (INT)m) * stride, n * stride, &pv, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) || !pv) goto fallback;
    memcpy(vbuf, pv, n * stride); IDirect3DVertexBuffer9_Unlock(vb); MKT(2);
    if (g_mkpat <= 0 || c != n / 2) { /* verify the quad index pattern (first 40 draws), or use the game's own indices */
        if (FAILED(IDirect3DIndexBuffer9_Lock(ib, st * 2, c * 3 * 2, &pi, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) || !pi) goto fallback;
        for (k = 0; k < c * 3; k++) { unsigned short ix = ((unsigned short *)pi)[k]; ibuf[k] = (unsigned short)(ix - m); } IDirect3DIndexBuffer9_Unlock(ib);
        if (g_mkpat == 0 && c == n / 2) { int okp = 1; for (q = 0; q < n / 4 && okp; q++) { unsigned short b0 = (unsigned short)(q * 4); if (ibuf[q * 6] != b0 || ibuf[q * 6 + 1] != b0 + 1 || ibuf[q * 6 + 2] != b0 + 2 || ibuf[q * 6 + 3] != b0 || ibuf[q * 6 + 4] != b0 + 2 || ibuf[q * 6 + 5] != b0 + 3) okp = 0; } if (!okp) { g_mkpat = -1; logmsg("mk_draw: index pattern differs, fast path off"); } else if (++g_mkchk >= 40) { g_mkpat = 1; logmsg("mk_draw: quad index pattern verified, fast path on"); } }
    } else { for (q = 0; q < n / 4; q++) { unsigned short b0 = (unsigned short)(q * 4); ibuf[q * 6] = b0; ibuf[q * 6 + 1] = b0 + 1; ibuf[q * 6 + 2] = b0 + 2; ibuf[q * 6 + 3] = b0; ibuf[q * 6 + 4] = b0 + 2; ibuf[q * 6 + 5] = b0 + 3; } }
    MKT(2);
    for (q = 0; q < n / 4; q++) { /* a quad = 4 consecutive vertices (x y z rhw u v, stride 24): bottom = the two with the larger y */
        float *v4[4], ymin = 1e9f, ymax = -1e9f, vmin = 1e9f, vmax = -1e9f; int j; for (j = 0; j < 4; j++) { float *f = (float *)(vbuf + (size_t)(q * 4 + j) * 24); v4[j] = f; if (f[1] < ymin) ymin = f[1]; if (f[1] > ymax) ymax = f[1]; if (f[5] < vmin) vmin = f[5]; if (f[5] > vmax) vmax = f[5]; }
        if (ymax - ymin < 4.0f || vmax - vmin < 0.01f) continue;
        { float umin = 1e9f, umax = -1e9f, hh = (ymax - ymin) * 1440.0f / V.vh, lim = cfg_mkdesc * (hh > 70.0f ? 4.0f : hh > 40.0f ? 2.0f : 1.0f), ex, dv, dy; int jj, tr;
          for (jj = 0; jj < 4; jj++) { float uu = v4[jj][4]; if (uu < umin) umin = uu; if (uu > umax) umax = uu; }
          tr = mk_tail_rows_c(ai, (int)(umin * g_atl[ai].w), (int)(umax * g_atl[ai].w + 0.999f), (int)(vmax * g_atl[ai].h + 0.5f), (int)(lim + 0.999f) + 1); /* rows of this glyph's own ink below the cut */
          if (hh > 38.0f && cfg_textdebug) { static int nd; static float seen[64][4]; int sk = 0, q9; for (q9 = 0; q9 < nd && q9 < 64; q9++) if (seen[q9][0] == (float)ai && seen[q9][1] == umin && seen[q9][2] == umax && seen[q9][3] == vmax) sk = 1; if (!sk && nd < 64) { seen[nd][0] = (float)ai; seen[nd][1] = umin; seen[nd][2] = umax; seen[nd][3] = vmax; nd++; } if (!sk) logmsg("TD atlas %d %ux%u quad y %.0f..%.0f (hh %.0f) u %.0f..%.0f v %.1f..%.1f tail(lim %d)=%d tail(30)=%d tail(60)=%d", ai, g_atl[ai].w, g_atl[ai].h, ymin, ymax, hh, umin * g_atl[ai].w, umax * g_atl[ai].w, vmin * g_atl[ai].h, vmax * g_atl[ai].h, (int)(lim + 0.999f) + 1, tr, mk_tail_rows(ai, (int)(umin * g_atl[ai].w), (int)(umax * g_atl[ai].w + 0.999f), (int)(vmax * g_atl[ai].h + 0.5f), 30), mk_tail_rows(ai, (int)(umin * g_atl[ai].w), (int)(umax * g_atl[ai].w + 0.999f), (int)(vmax * g_atl[ai].h + 0.5f), 60)); }
          if (tr <= 0) continue; ex = (float)tr; if (ex > lim) ex = lim; dv = ex / 256.0f; dy = ex / 256.0f * (ymax - ymin) / (vmax - vmin);
          for (j = 0; j < 4; j++) if (v4[j][1] >= ymax - 0.01f) { v4[j][1] += dy; v4[j][5] += dv; } }
    }
    MKT(3);
    if (g_mkpat > 0 && c == n / 2 && n <= 256 && mk_ring_make(d)) { void *rp = NULL; UINT bytes = n * 24; DWORD fl = D3DLOCK_NOOVERWRITE; if (g_mkoff + bytes > MKRING) { g_mkoff = 0; fl = D3DLOCK_DISCARD; }
        if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(g_mkvb, g_mkoff, bytes, &rp, fl)) && rp) { memcpy(rp, vbuf, bytes); IDirect3DVertexBuffer9_Unlock(g_mkvb); IDirect3DDevice9_SetStreamSource(d, 0, g_mkvb, 0, 24); IDirect3DDevice9_SetIndices(d, g_mkib); hr = o_DIP(d, t, (INT)(g_mkoff / 24), 0, n, 0, c); g_mkoff += bytes; ok = 1; goto restore; } }
    hr = o_DIPUP(d, t, 0, n, c, ibuf, D3DFMT_INDEX16, vbuf, 24); ok = 1;
restore:
    MKT(4);
    IDirect3DDevice9_SetStreamSource(d, 0, vb, off, stride); IDirect3DDevice9_SetIndices(d, ib);
    MKT(5);
fallback:
    if (vb) IDirect3DVertexBuffer9_Release(vb); if (ib) IDirect3DIndexBuffer9_Release(ib);
    MKT(6);
    return ok ? hr : o_DIP(d, t, b, m, n, st, c);
}
static HRESULT WINAPI h_DIP_inner(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT n, UINT s, UINT c) { HRESULT hr; g_mrt_dev = d; mrt_log("DIP", V.fvf, n); mrt_ib_log(d, b, m, n, s, c); mrt_3d_log(d, b, m, n); mrt_state_log(d, b, m, n); mrt_states(d, V.fvf); if (g_mrt && n == 4 && !g_in_post && g_mrt_idx == 0 && getf("MrtTexNull", 0.0f) > 0.5f) { IDirect3DDevice9_SetTexture(d, 0, NULL); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); } if (g_mrt && n == 4 && rhw_fvf(V.fvf) && !g_in_post && cfg_mrtcpu > 0.5f) { IDirect3DVertexBuffer9 *vb9 = NULL; UINT off9 = 0, st9 = 0; void *pv9 = NULL; if (SUCCEEDED(IDirect3DDevice9_GetStreamSource(d, 0, &vb9, &off9, &st9)) && vb9) { if (st9 <= 64 && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb9, off9 + (UINT)(b + (INT)m) * st9, 4 * st9, &pv9, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && pv9) { char tmp9[4 * 64]; int q; memcpy(tmp9, pv9, 4 * st9); IDirect3DVertexBuffer9_Unlock(vb9); for (q = 0; q < 4; q++) { float *f9 = (float *)(tmp9 + q * st9); f9[0] *= cfg_mrts; f9[1] *= cfg_mrts; } g_in_post = 1; if (cfg_mrtcpu > 1.5f) { static const struct { D3DRENDERSTATETYPE r; DWORD v; } rs[] = { {D3DRS_ALPHATESTENABLE, 0}, {D3DRS_FOGENABLE, 0}, {D3DRS_STENCILENABLE, 0}, {D3DRS_ZENABLE, 0}, {D3DRS_SCISSORTESTENABLE, 0}, {D3DRS_CLIPPING, 1}, {D3DRS_COLORWRITEENABLE, 15}, {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_LIGHTING, 0}, {D3DRS_SPECULARENABLE, 0}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_ZWRITEENABLE, 0}, {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_COLORVERTEX, 1} }; int q2; for (q2 = 0; q2 < (int)(sizeof rs / sizeof rs[0]); q2++) IDirect3DDevice9_SetRenderState(d, rs[q2].r, rs[q2].v); } HRESULT hr9 = o_DPUP(d, D3DPT_TRIANGLEFAN, 2, tmp9, st9); g_in_post = 0; IDirect3DDevice9_SetStreamSource(d, 0, vb9, off9, st9); IDirect3DVertexBuffer9_Release(vb9); return hr9; } IDirect3DVertexBuffer9_Release(vb9); } } if (mrt_skip()) return D3D_OK; dbg7(d, "DIP", c); if (PROBING()) { char bb[200]; log_draw(d, "DIP", t, c, NULL, 0); if (rhw_fvf(V.fvf)) { g_in_post = 1; vb_bbox(d, b, m, n, bb, sizeof bb); g_in_post = 0; if (bb[0]) plog("%s", bb); } } if (V_ACTIVE(V.fvf) && !g_skipvp) { float bb[4]; int hb = bounds_vb(d, b, m, n, bb); if (v_prepare(d, V.fvf, hb ? bb : NULL, n)) { hr = mk_draw(d, t, b, m, n, s, c); v_finish(d, V.fvf); return hr; } } return mk_draw(d, t, b, m, n, s, c); }

static int g_thumbdraw; static int g_dd_n;
static HRESULT WINAPI h_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT n, UINT s, UINT c)
{
    if (!g_mrt && !g_in_post && cfg_dbg == 6 && g_mrt_tex && V.fvf != 0) { char tc[MAX_PATH]; strcpy(tc, g_logp); strcpy(strrchr(tc, (char)92) + 1, "TheMoviesGfx.compdump.now"); if (GetFileAttributesA(tc) != INVALID_FILE_ATTRIBUTES) { IDirect3DBaseTexture9 *bt = NULL; if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, 0, &bt)) && bt) { D3DSURFACE_DESC dq; IDirect3DSurface9 *sf = NULL; if (o_TGLD && SUCCEEDED(o_TGLD((IDirect3DTexture9 *)bt, 0, &dq)) && dq.Width == g_mrt_px && (dq.Usage & D3DUSAGE_RENDERTARGET) && SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)bt, 0, &sf)) && sf) { logmsg("compdump: texture %p %ux%u surface %p", (void *)bt, dq.Width, dq.Height, (void *)sf); mrt_dump2(d, sf, "TheMoviesGfx.compdump.now", "TheMoviesGfx.compdump.bmp"); IDirect3DSurface9_Release(sf); } IDirect3DBaseTexture9_Release(bt); } } }
    if (g_mrt && !g_in_post && g_mrt_syncdraw) { static IDirect3DQuery9 *q2; HRESULT hrr = h_DIP_inner(d, t, b, m, n, s, c); if (!q2) IDirect3DDevice9_CreateQuery(d, D3DQUERYTYPE_EVENT, &q2); if (q2) { BOOL done = FALSE; int tries = 0; IDirect3DQuery9_Issue(q2, D3DISSUE_END); while (IDirect3DQuery9_GetData(q2, &done, sizeof done, D3DGETDATA_FLUSH) == S_FALSE && tries++ < 2000) Sleep(0); } return hrr; }
    if (cfg_dbg == 6 && !g_in_post && n == 4 && V.fvf == 0x144) { float bb9[4]; if (bounds_vb(d, b, m, n, bb9) && bb9[2] > 1328.0f && bb9[3] <= 1441.0f && bb9[0] < 1700.0f && (bb9[1] - bb9[0]) > 40.0f) { IDirect3DBaseTexture9 *bt = NULL; if (SUCCEEDED(IDirect3DDevice9_GetTexture(d, 0, &bt)) && bt) { D3DSURFACE_DESC dq; IDirect3DTexture9 *tx = (IDirect3DTexture9 *)bt; if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(tx, 0, &dq)) && dq.Width >= 100 && dq.Height >= 32 && (bb9[1] - bb9[0]) < 600.0f) { static int nlt; static void *seen[8]; static int nseen; int sn = 0, q9; for (q9 = 0; q9 < nseen; q9++) if (seen[q9] == (void *)bt) sn = 1; if (!sn && nseen < 8) { D3DLOCKED_RECT lr9; seen[nseen] = (void *)bt; if (dq.Pool != D3DPOOL_DEFAULT && SUCCEEDED(IDirect3DTexture9_LockRect(tx, 0, &lr9, NULL, D3DLOCK_READONLY))) { char tp[MAX_PATH]; FILE *tf; snprintf(tp, sizeof tp, "%s", g_logp); { char nm[40]; snprintf(nm, sizeof nm, "TheMoviesGfx.thumb%d.raw", nseen); strcpy(strrchr(tp, (char)92) + 1, nm); } if ((tf = fopen(tp, "wb"))) { fwrite(&dq.Width, 4, 1, tf); fwrite(&dq.Height, 4, 1, tf); fwrite(&dq.Format, 4, 1, tf); fwrite(&lr9.Pitch, 4, 1, tf); fwrite(lr9.pBits, 1, (size_t)(dq.Width / 4) * (dq.Height / 4) * 16, tf); fclose(tf); } IDirect3DTexture9_UnlockRect(tx, 0); } nseen++; } if (dq.Format == 0x31545844 && dq.Width == 256 && dq.Height == 128) { g_thumbdraw = 1; if (getf("ThumbNoVS", 0.0f) > 0.5f) g_skipvp = 1; if (getf("ThumbDirty", 0.0f) > 0.5f) IDirect3DTexture9_AddDirtyRect(tx, NULL); } if (dq.Format == 0x31545844 && dq.Width == 256 && dq.Height == 128) { static int nlv2; if (nlv2++ < 3) { IDirect3DVertexBuffer9 *vb2 = NULL; UINT off2 = 0, st2 = 0; void *pv2 = NULL; DWORD rz, rzw, rzf, rab, rsb, rdb, rcw, rat, rsc, rcu; DWORD co, c1, c2, ao, a1, a2, mn, mg, mp, au, av; if (SUCCEEDED(IDirect3DDevice9_GetStreamSource(d, 0, &vb2, &off2, &st2)) && vb2) { if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb2, off2 + (UINT)(b + (INT)m) * st2, 4 * st2, &pv2, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && pv2) { UINT q2; for (q2 = 0; q2 < 4; q2++) { const float *f2 = (const float *)((const char *)pv2 + q2 * st2); logmsg("THUMBV %u: %.1f %.1f %.2f %.2f col %08lx uv %.3f %.3f", q2, f2[0], f2[1], f2[2], f2[3], *(const unsigned long *)(f2 + 4), f2[5], f2[6]); } IDirect3DVertexBuffer9_Unlock(vb2); } IDirect3DVertexBuffer9_Release(vb2); }
 IDirect3DDevice9_GetRenderState(d, D3DRS_ZENABLE, &rz); IDirect3DDevice9_GetRenderState(d, D3DRS_ZWRITEENABLE, &rzw); IDirect3DDevice9_GetRenderState(d, D3DRS_ZFUNC, &rzf); IDirect3DDevice9_GetRenderState(d, D3DRS_ALPHABLENDENABLE, &rab); IDirect3DDevice9_GetRenderState(d, D3DRS_SRCBLEND, &rsb); IDirect3DDevice9_GetRenderState(d, D3DRS_DESTBLEND, &rdb); IDirect3DDevice9_GetRenderState(d, D3DRS_COLORWRITEENABLE, &rcw); IDirect3DDevice9_GetRenderState(d, D3DRS_ALPHATESTENABLE, &rat); IDirect3DDevice9_GetRenderState(d, D3DRS_SCISSORTESTENABLE, &rsc); IDirect3DDevice9_GetRenderState(d, D3DRS_CULLMODE, &rcu);
 IDirect3DDevice9_GetTextureStageState(d, 0, D3DTSS_COLOROP, &co); IDirect3DDevice9_GetTextureStageState(d, 0, D3DTSS_COLORARG1, &c1); IDirect3DDevice9_GetTextureStageState(d, 0, D3DTSS_COLORARG2, &c2); IDirect3DDevice9_GetTextureStageState(d, 0, D3DTSS_ALPHAOP, &ao); IDirect3DDevice9_GetTextureStageState(d, 0, D3DTSS_ALPHAARG1, &a1); IDirect3DDevice9_GetTextureStageState(d, 0, D3DTSS_ALPHAARG2, &a2); IDirect3DDevice9_GetSamplerState(d, 0, D3DSAMP_MINFILTER, &mn); IDirect3DDevice9_GetSamplerState(d, 0, D3DSAMP_MAGFILTER, &mg); IDirect3DDevice9_GetSamplerState(d, 0, D3DSAMP_MIPFILTER, &mp); IDirect3DDevice9_GetSamplerState(d, 0, D3DSAMP_ADDRESSU, &au); IDirect3DDevice9_GetSamplerState(d, 0, D3DSAMP_ADDRESSV, &av);
 logmsg("THUMBST z%lu zw%lu zf%lu ab%lu src%lu dst%lu cw%lx at%lu sc%lu cull%lu | cop%lu c1 %lx c2 %lx aop%lu a1 %lx a2 %lx | min%lu mag%lu mip%lu au%lu av%lu | scissor", rz, rzw, rzf, rab, rsb, rdb, rcw, rat, rsc, rcu, co, c1, c2, ao, a1, a2, mn, mg, mp, au, av); } }
if (nlt++ < 40) logmsg("THUMB tex %p %ux%u fmt %lx usage %lx pool %d lv %lu at x %.0f..%.0f", (void *)bt, dq.Width, dq.Height, (unsigned long)dq.Format, (unsigned long)dq.Usage, (int)dq.Pool, (unsigned long)IDirect3DBaseTexture9_GetLevelCount(bt), bb9[0], bb9[1]); } IDirect3DBaseTexture9_Release(bt); } } }
    HRESULT hr = h_DIP_inner(d, t, b, m, n, s, c); g_skipvp = 0;
    if (g_thumbdraw) { g_thumbdraw = 0; { IDirect3DSurface9 *rt9 = NULL; if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(d, 0, &rt9)) && rt9) { mrt_dump2(d, rt9, "TheMoviesGfx.thumbdump.now", "TheMoviesGfx.thumb_after.bmp"); IDirect3DSurface9_Release(rt9); } } }
    if (g_mrt && g_mrt_surf && g_mrt_passno == g_mrt_moviepass && !g_in_post) { char tp[MAX_PATH]; strcpy(tp, g_logp); strcpy(strrchr(tp, (char)92) + 1, "TheMoviesGfx.drawdump.now"); if (GetFileAttributesA(tp) != INVALID_FILE_ATTRIBUTES) { char on[64]; snprintf(on, sizeof on, "TheMoviesGfx.dd%02d.bmp", g_dd_n++); { char tt[MAX_PATH]; strcpy(tt, g_logp); strcpy(strrchr(tt, (char)92) + 1, "TheMoviesGfx.drawdump.keep"); FILE *kf = fopen(tt, "w"); if (kf) fclose(kf); } mrt_dump2(d, g_mrt_surf, "TheMoviesGfx.drawdump.keep", on); } }
    return hr;
}

static HRESULT WINAPI h_DPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT c, const void *v, UINT s) { HRESULT hr; g_mrt_dev = d; mrt_log("DPUP", V.fvf, c); dbg7(d, "DPUP", c); if (PROBING()) log_draw(d, "DPUP", t, c, v, s); if (V_ACTIVE(V.fvf)) { float bb[4]; UINT nv = (t == D3DPT_TRIANGLELIST) ? c * 3 : (t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN) ? c + 2 : 0; int hb = bounds_mem(v, s, nv, bb); if (v_prepare(d, V.fvf, hb ? bb : NULL, nv)) { hr = o_DPUP(d, t, c, v, s); v_finish(d, V.fvf); return hr; } } return o_DPUP(d, t, c, v, s); }
static HRESULT WINAPI h_DIPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT m, UINT n, UINT c, const void *i, D3DFORMAT f, const void *v, UINT s) { HRESULT hr; g_mrt_dev = d; mrt_log("DIPUP", V.fvf, n); dbg7(d, "DIPUP", c); if (PROBING()) log_draw(d, "DIPUP", t, c, v, s); if (V_ACTIVE(V.fvf)) { float bb[4]; int hb = bounds_mem((const char *)v + (size_t)m * s, s, n, bb); if (v_prepare(d, V.fvf, hb ? bb : NULL, n)) { hr = o_DIPUP(d, t, m, n, c, i, f, v, s); v_finish(d, V.fvf); return hr; } } return o_DIPUP(d, t, m, n, c, i, f, v, s); }
static HRESULT WINAPI h_VP(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v)
{ mrt_seq("SetViewport");
    if (PROBING() && v) plog("SetViewport %lu,%lu %lux%lu z%.2f..%.2f", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
    if (g_mrt && cfg_dbg == 6) logmsg("MRT SetViewport %lu,%lu %lux%lu", v->X, v->Y, v->Width, v->Height);
    if (g_mrt && v && !g_in_post) { D3DVIEWPORT9 sv = *v; sv.X = (DWORD)(v->X * cfg_mrts + 0.5f); sv.Y = (DWORD)(v->Y * cfg_mrts + 0.5f); sv.Width = (DWORD)(v->Width * cfg_mrts + 0.5f); sv.Height = (DWORD)(v->Height * cfg_mrts + 0.5f); return o_VP(d, &sv); }
    if (V.on && V.rt_bb && v && !g_in_post) { D3DVIEWPORT9 sv = *v; sv.X = v->X * V.pw / V.vw; sv.Y = v->Y * V.ph / V.vh; sv.Width = v->Width * V.pw / V.vw; sv.Height = v->Height * V.ph / V.vh; return o_VP(d, &sv); }
    return o_VP(d, v);
}
static HRESULT WINAPI h_SC(IDirect3DDevice9 *d, const RECT *r)
{ mrt_seq("SetScissor");
    if (PROBING() && r) plog("SetScissorRect %ld,%ld,%ld,%ld", r->left, r->top, r->right, r->bottom);
    if (g_mrt && cfg_dbg == 6 && r) logmsg("MRT SetScissorRect %ld,%ld,%ld,%ld", r->left, r->top, r->right, r->bottom);
    if (g_mrt && r && !g_in_post) { RECT sr = {(LONG)(r->left * cfg_mrts + 0.5f), (LONG)(r->top * cfg_mrts + 0.5f), (LONG)(r->right * cfg_mrts + 0.5f), (LONG)(r->bottom * cfg_mrts + 0.5f)}; return o_SC(d, &sr); }
    if (V.on && V.rt_bb && r && !g_in_post) { RECT sr = {(LONG)((long long)r->left * V.pw / V.vw), (LONG)((long long)r->top * V.ph / V.vh), (LONG)((long long)r->right * V.pw / V.vw), (LONG)((long long)r->bottom * V.ph / V.vh)}; return o_SC(d, &sr); }
    return o_SC(d, r);
}
static void mrt_dump2(IDirect3DDevice9 *d, IDirect3DSurface9 *src, const char *trig, const char *outn);
/* Movie render resolution: the game keeps its "render surface" size in two globals (0xe67ba0/4) set by a function at 0x9a63a0(w, h=-1); the movie code calls it with 512. Patch those immediates (push 0x200) to MovieRes. */
volatile unsigned long g_mr_allow;
void __cdecl __attribute__((used)) mr_filter(unsigned long ret, unsigned long a1, unsigned long a2)
{
    /* called at the entry of the game's set-render-size function 0x9a63a0 (w=a1, h=a2, -1 = same as w): decides whether this call is the movie viewer's */
    static unsigned long seen[64]; static int ns; int i, known = 0;
    for (i = 0; i < ns; i++) if (seen[i] == ret) known = 1;
    if (!known && ns < 64) { seen[ns++] = ret; logmsg("set-render-size call from %lx (w %ld h %ld) frame %lu player-screen %d", ret, (long)a1, (long)a2, G.frame, (int)(G.frame - g_pl_sig <= 30)); }
    g_mr_allow = (ret == 0x7841f8 || ret == 0x77dc1d) || g_mr_always; /* the movie viewer's calls; the studio / boot / menus keep 512 */
}
static void __attribute__((naked)) movie_res_stub(void)
{
    /* entry of the game's set-render-size function (w=[esp+4], h=[esp+8], h may be -1): replace a 512 by MovieRes when allowed, then run the stolen instructions and continue at 0x9a63a5 */
    __asm__ __volatile__(
        "pushl %eax\n\t" "pushl %ecx\n\t" "pushl %edx\n\t"
        "pushl 20(%esp)\n\t" "pushl 20(%esp)\n\t" "pushl 20(%esp)\n\t" "call _mr_filter\n\t" "addl $12, %esp\n\t"
        "popl %edx\n\t" "popl %ecx\n\t" "popl %eax\n\t"
        "cmpl $0, _g_mr_allow\n\t" "je 9f\n\t"
        "cmpl $512, 4(%esp)\n\t" "jne 1f\n\t" "pushl %eax\n\t" "movl _g_mr_res, %eax\n\t" "movl %eax, 8(%esp)\n\t" "popl %eax\n\t" "1:\n\t"
        "cmpl $512, 8(%esp)\n\t" "jne 9f\n\t" "pushl %eax\n\t" "movl _g_mr_res, %eax\n\t" "movl %eax, 12(%esp)\n\t" "popl %eax\n\t"
        "9:\n\t"
        "pushl %esi\n\t" "movl 12(%esp), %esi\n\t" "movl $0x9a63a5, %eax\n\t" "jmp *%eax\n\t");
}
/* ---- generic widget edge setters (left 0x741940, right 0x741a50, top 0x741b60, bottom 0x741c70; thiscall, 3 stack args: kind, from, offset) ---- */
volatile int g_edge_on; static int g_edge_n;
/* Movie Player bottom buttons, scaled where the game builds them (function 0x77e8xx-0x7817xx): sizes are push-immediates of the button constructor (0x69fb10) patched by plbtn_patch(); positions are the top/right offsets handed to the setters, rewritten here per call site (ret = address after the call). Tops hang from the bottom band (1308) and grow upward by h0*(z-1); the centre playback button keeps its centre (R offset - w0*(z-1)/2) */
static const struct { unsigned long ret; char w; float h0; } g_plb_edge[] = {
    { 0x77f000, 'T', 40 }, { 0x77f22c, 'T', 40 }, { 0x77f786, 'T', 40 }, { 0x77f9ac, 'T', 32 }, { 0x77fbb9, 'T', 32 }, { 0x77fdc6, 'T', 32 }, { 0x77ffd3, 'T', 32 },
    { 0x7801e2, 'T', 40 }, { 0x7803e5, 'T', 40 }, { 0x7805d2, 'T', 40 }, { 0x7807d5, 'T', 40 },
    { 0x77f76d, 'R', 40 } };
static unsigned long g_plb_self[16]; static volatile int g_plb_nself; /* widgets (this) the build hook has seen: the runtime scaler leaves them alone */
static const unsigned long g_plb_imm[] = { 0x77eeb7, 0x77eec5, 0x77f0e2, 0x77f0f0, 0x77f62a, 0x77f62f, 0x77f863, 0x77f871, 0x77fa6b, 0x77fa79, 0x77fc78, 0x77fc86, 0x77fe85, 0x77fe93,
    0x780092, 0x7800a0, 0x7802a1, 0x7802af, 0x78048b, 0x780495, 0x780692, 0x7806a0 };
void __cdecl __attribute__((used)) edge_log(int which, unsigned long self, unsigned long ret, unsigned long a1, unsigned long a2, unsigned long a3, float *pa3)
{
    float f2 = *(float *)&a2, f3 = *(float *)&a3;
    if (cfg_plbtn > 1.0f && ret >= 0x77e000 && ret < 0x781900) { int i; for (i = 0; i < (int)(sizeof g_plb_edge / sizeof g_plb_edge[0]); i++) if (g_plb_edge[i].ret == ret) { int q; if (ret == 0x77f000) g_plb_nself = 0; for (q = 0; q < g_plb_nself; q++) if (g_plb_self[q] == self) break; if (q == g_plb_nself && g_plb_nself < 16) g_plb_self[g_plb_nself++] = self; *pa3 = f3 - (g_plb_edge[i].w == 'T' ? 1.0f : 0.5f) * (cfg_plbtn - 1.0f) * g_plb_edge[i].h0; break; } }
    if (!g_edge_on || g_edge_n > 6000) return; g_edge_n++;
    logmsg("EDGE %c this %lx ret %lx a1 %lx a2 %lx (%.2f) a3 %lx (%.2f)", which, self, ret, a1, a2, f2, a3, f3);
}
static unsigned long __cdecl export_width_fn(void)
{
    /* replaces the game's 'mov eax, 0x300' at 0x9720b0: width of the export capture target and frames (height = width*9/16, see 0xa2b7b0). The capture target is created when the player opens, before a quality level is chosen, so one width serves all levels; the quality profile scales the frames down to its own size */
    return g_exp_w[4];
}
static void export_width_patch(void)
{
    unsigned char *f = (unsigned char *)0x9720b0; DWORD old; static int done; if (done) return; done = 1;
    if (!IsBadReadPtr(f, 6) && f[0] == 0xb8 && *(unsigned long *)(f + 1) == 0x300 && f[5] == 0xc3 && VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old)) { f[0] = 0xe9; *(long *)(f + 1) = (long)((unsigned char *)export_width_fn - (f + 5)); VirtualProtect(f, 5, old, &old); logmsg("ExportPresets: export capture width hooked -> %lu", g_exp_w[4]); }
    else logmsg("ExportPresets: 0x9720b0 does not match");
}
static void movie_res_patch(void)
{
    static const unsigned long sites[] = { 0x7617a5, 0x7637d5, 0x77711d, 0x7841ee }; int i; unsigned long res = (unsigned long)getf("MovieRes", 512.0f);
    static int done; if (done || res == 512 || res < 256 || res > 4096) return; done = 1;
    { unsigned char *f = (unsigned char *)0x9a63a0; DWORD old2; g_mr_res = res; if (!IsBadReadPtr(f, 5) && f[0] == 0x56 && f[1] == 0x8b && f[2] == 0x74 && f[3] == 0x24 && f[4] == 0x0c && VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old2)) { f[0] = 0xe9; *(long *)(f + 1) = (long)((unsigned char *)movie_res_stub - (f + 5)); VirtualProtect(f, 5, old2, &old2); logmsg("MovieRes: hooked set-render-size 0x9a63a0 -> %lu", res); } else logmsg("MovieRes: 0x9a63a0 does not match"); }
    return; /* the hook replaces the per-site patches */
    for (i = 0; i < 4; i++) { unsigned char *p = (unsigned char *)sites[i]; DWORD old; if (IsBadReadPtr(p, 5) || p[0] != 0x68 || *(unsigned long *)(p + 1) != 0x200) { logmsg("MovieRes: site %lx does not match (%02x %lx)", sites[i], p[0], *(unsigned long *)(p + 1)); continue; } if (VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { *(unsigned long *)(p + 1) = res; VirtualProtect(p + 1, 4, old, &old); logmsg("MovieRes: patched %lx -> %lu", sites[i], res); } }
}
static void mrt_dump(IDirect3DDevice9 *d, IDirect3DSurface9 *src) { mrt_dump2(d, src, "TheMoviesGfx.rtdump.now", "TheMoviesGfx.rtdump.bmp"); }
static void mrt_dump2(IDirect3DDevice9 *d, IDirect3DSurface9 *src, const char *trig, const char *outn)
{
    char t[MAX_PATH]; D3DSURFACE_DESC dd; IDirect3DSurface9 *sys = NULL; D3DLOCKED_RECT lr; FILE *f;
    strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, trig); if (GetFileAttributesA(t) == INVALID_FILE_ATTRIBUTES) return; DeleteFileA(t);
    if (FAILED(real_desc(src, &dd))) return;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, dd.Width, dd.Height, dd.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys) { logmsg("rtdump: no sysmem surface"); return; }
    { HRESULT hq = IDirect3DDevice9_GetRenderTargetData(d, src, sys); if (FAILED(hq)) logmsg("rtdump: GetRenderTargetData failed %lx (%ux%u fmt %d usage %lx pool %d)", (unsigned long)hq, dd.Width, dd.Height, (int)dd.Format, (unsigned long)dd.Usage, (int)dd.Pool); if (SUCCEEDED(hq) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, outn);
        if ((f = fopen(t, "wb"))) { unsigned hdr[14] = {0}; unsigned char *h = (unsigned char *)hdr; unsigned long sz = 54 + dd.Width * dd.Height * 4; UINT y; memset(h, 0, 56); h[0] = 'B'; h[1] = 'M'; *(unsigned long *)(h + 2) = sz; *(unsigned long *)(h + 10) = 54; *(unsigned long *)(h + 14) = 40; *(long *)(h + 18) = dd.Width; *(long *)(h + 22) = -(long)dd.Height; *(unsigned short *)(h + 26) = 1; *(unsigned short *)(h + 28) = 32; fwrite(h, 1, 54, f); for (y = 0; y < dd.Height; y++) fwrite((char *)lr.pBits + (size_t)y * lr.Pitch, 1, dd.Width * 4, f); fclose(f); logmsg("rtdump: wrote %ux%u", dd.Width, dd.Height); }
        IDirect3DSurface9_UnlockRect(sys);
    } }
    IDirect3DSurface9_Release(sys);
}
static HRESULT WINAPI h_RT(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{
    if (g_mrt && i == 0 && !g_in_post && getf("MrtSync", 0.0f) > 0.5f) { static IDirect3DQuery9 *q; if (!q) IDirect3DDevice9_CreateQuery(d, D3DQUERYTYPE_EVENT, &q); if (q) { BOOL done = FALSE; int tries = 0; IDirect3DQuery9_Issue(q, D3DISSUE_END); while (IDirect3DQuery9_GetData(q, &done, sizeof done, D3DGETDATA_FLUSH) == S_FALSE && tries++ < 2000) Sleep(0); } } /* debug: GPU sync at the end of each movie pass */
    if (g_mrt && g_mrt_surf && s != g_mrt_surf && i == 0) { if (g_mrt_movie && getf("MrtTest", 0.0f) > 1.5f) { struct { float x, y, z, rhw; DWORD c; } tv[4] = { {50, 50, 0, 1, 0xffff0000}, {250, 50, 0, 1, 0xffff0000}, {250, 200, 0, 1, 0xff00ff00}, {50, 200, 0, 1, 0xff00ff00} }; if (v_prepare(d, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, NULL, 4)) { IDirect3DDevice9_SetTexture(d, 0, NULL); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); o_DPUP(d, D3DPT_TRIANGLEFAN, 2, tv, sizeof tv[0]); v_finish(d, D3DFVF_XYZRHW | D3DFVF_DIFFUSE); } }
    if (g_mrt_movie && getf("MrtTest", 0.0f) > 0.5f && getf("MrtTest", 0.0f) < 1.5f) { struct { float x, y, z, rhw; DWORD c; } tv[4] = { {100, 100, 0, 1, 0xffff0000}, {500, 100, 0, 1, 0xffff0000}, {500, 400, 0, 1, 0xff00ff00}, {100, 400, 0, 1, 0xff00ff00} }; g_in_post = 1; IDirect3DDevice9_SetVertexShader(d, NULL); IDirect3DDevice9_SetFVF(d, D3DFVF_XYZRHW | D3DFVF_DIFFUSE); IDirect3DDevice9_SetTexture(d, 0, NULL); IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE); IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); o_DPUP(d, D3DPT_TRIANGLEFAN, 2, tv, sizeof tv[0]); g_in_post = 0; } if (g_mrt_movie) { static int nlp; if (cfg_dbg == 6 && nlp++ < 3) { int q; for (q = 0; q < g_mrt_nall; q++) logmsg("MOVIEPASS all %d: fvf 0x%lx n %lu", q, g_mrt_all[q][0], g_mrt_all[q][1]); for (q = 0; q < g_mrt_npd; q++) logmsg("MOVIEPASS draw %d: x %.0f..%.0f y %.0f..%.0f nv %.0f", q, g_mrt_pd[q][0], g_mrt_pd[q][1], g_mrt_pd[q][2], g_mrt_pd[q][3], g_mrt_pd[q][4]); } mrt_dump(d, g_mrt_surf); } g_mrt_movie = 0; }
    if (s && i == 0) { D3DSURFACE_DESC dd; real_desc(s, &dd); g_mrt = ((cfg_mrts > 1.0f && dd.Width == g_mrt_px && dd.Height == g_mrt_px) || (g_mr_res > 512 && dd.Width == g_mr_res && dd.Height == g_mr_res && (dd.Usage & D3DUSAGE_RENDERTARGET))); if (g_mrt) g_mrt_surf = s; { static int nl3; if (cfg_dbg == 6 && nl3++ < 100) logmsg("SetRenderTarget -> %ux%u usage 0x%lx fmt %d mrt %d caller %p", dd.Width, dd.Height, (unsigned long)dd.Usage, (int)dd.Format, g_mrt, __builtin_return_address(0)); } if (!g_in_post) V.rt_bb = (dd.Width == V.pw && dd.Height == V.ph); if (PROBING()) plog("SetRenderTarget %lu -> %ux%u fmt%d usage 0x%lx (%s)", i, dd.Width, dd.Height, dd.Format, dd.Usage, V.rt_bb ? "back buffer, UI scaling applies" : "off-screen, unscaled"); }
    { HRESULT hr0 = o_RT(d, i, s);
      if (i == 0 && !g_in_post) { /* an off-screen target (movie playback / export capture) larger than the device's depth buffer needs a depth surface of its own, or depth testing is wrong where it does not fit */
          static IDirect3DSurface9 *dsc[4]; static UINT dsc_px[4]; static IDirect3DSurface9 *odss; static int bound;
          D3DSURFACE_DESC rd; UINT need = 0;
          if (s && SUCCEEDED(real_desc(s, &rd)) && (rd.Usage & D3DUSAGE_RENDERTARGET) && !(rd.Width == V.pw && rd.Height == V.ph)) need = rd.Width > rd.Height ? rd.Width : rd.Height;
          if (need > 512 && !g_dbg_nodepth) {
              IDirect3DSurface9 *cur = NULL; static UINT boundpx;
              if ((!bound || need > boundpx) && (bound ? (cur = odss, odss = NULL, cur != NULL) : (SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface(d, &cur)) && cur))) {
                  D3DSURFACE_DESC dd; int wasb = bound; real_desc(cur, &dd); if (bound) { bound = 0; }
                  if (dd.Width < need || dd.Height < need) {
                      int k, use = -1; for (k = 0; k < 4; k++) if (dsc[k] && dsc_px[k] == need) use = k;
                      if (use < 0) { for (k = 0; k < 4 && dsc[k]; k++) ; if (k == 4) { k = 0; IDirect3DSurface9_Release(dsc[0]); dsc[0] = NULL; } if (SUCCEEDED(IDirect3DDevice9_CreateDepthStencilSurface(d, need, need, dd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &dsc[k], NULL)) && dsc[k]) { dsc_px[k] = need; use = k; logmsg("depth surface %ux%u fmt %d created for a %ux%u target", need, need, (int)dd.Format, rd.Width, rd.Height); } }
                      if (use >= 0) { odss = cur; IDirect3DDevice9_SetDepthStencilSurface(d, dsc[use]); bound = 1; boundpx = need; cur = NULL; }
                  }
                  if (cur) { if (wasb && bound == 0) IDirect3DDevice9_SetDepthStencilSurface(d, cur); IDirect3DSurface9_Release(cur); }
              }
          } else if (bound) { if (odss) { IDirect3DDevice9_SetDepthStencilSurface(d, odss); IDirect3DSurface9_Release(odss); odss = NULL; } bound = 0; }
      }
      if (g_mrt && i == 0 && cfg_dbg == 6 && getf("MrtNoDS", 0.0f) > 0.5f) IDirect3DDevice9_SetDepthStencilSurface(d, NULL); return hr0; }
}
static HRESULT WINAPI h_TR(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    if (PROBING() && m && (t == D3DTS_PROJECTION)) plog("SetTransform PROJECTION m00=%.4f m11=%.4f m22=%.4f m23=%.4f m33=%.4f m30=%.3f m31=%.3f %s", m->_11, m->_22, m->_33, m->_34, m->_44, m->_41, m->_42, m->_44 == 1.0f ? "(orthographic)" : "(perspective)");
    return o_TR(d, t, m);
}
static HRESULT WINAPI h_CLR(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s)
{
    if (PROBING()) plog("Clear flags=0x%lx rects=%lu color=%08lx", f, n, (unsigned long)c);
    if (g_mrt) { if (g_mrt_passframe != G.frame) { g_mrt_passframe = G.frame; g_mrt_passno = -1; } g_mrt_passno++; if (g_mrt_passno == g_mrt_moviepass) g_dd_n = 0; g_mrt_idx = 0; g_mrt_npd = 0; g_mrt_nall = 0; if ((f & D3DCLEAR_TARGET) && !(f & D3DCLEAR_ZBUFFER)) { f |= D3DCLEAR_ZBUFFER; z = 1.0f; } } /* the movie target shares the big depth buffer with everything else: a colour-only clear left stale depth in its corner (the 2D passes then failed the depth test there) */
    if (g_mrt && cfg_dbg == 6) logmsg("MRT Clear flags 0x%lx rects %lu color %08lx caller %p", f, n, (unsigned long)c, __builtin_return_address(0));
    if (g_mrt && !n && !g_in_post && (f & D3DCLEAR_TARGET) && getf("MrtDrawClear", 0.0f) > 0.5f) { IDirect3DStateBlock9 *sb = NULL; if (SUCCEEDED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &sb)) && sb) { struct { float x, y, z, rhw; DWORD c; } qv[4] = { {0, 0, 0, 1, c}, {(float)g_mrt_px, 0, 0, 1, c}, {(float)g_mrt_px, (float)g_mrt_px, 0, 1, c}, {0, (float)g_mrt_px, 0, 1, c} }; IDirect3DStateBlock9_Capture(sb); g_in_post = 1; IDirect3DDevice9_SetVertexShader(d, NULL); IDirect3DDevice9_SetPixelShader(d, NULL); IDirect3DDevice9_SetFVF(d, D3DFVF_XYZRHW | D3DFVF_DIFFUSE); IDirect3DDevice9_SetTexture(d, 0, NULL); IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE); IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE); IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE); IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE); IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE); IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 15); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE); IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE); o_DPUP(d, D3DPT_TRIANGLEFAN, 2, qv, sizeof qv[0]); IDirect3DStateBlock9_Apply(sb); g_in_post = 0; IDirect3DStateBlock9_Release(sb); f &= ~D3DCLEAR_TARGET; if (!f) return D3D_OK; } }
    if (g_mrt && !n && !g_in_post && g_mrt_surf && (f & D3DCLEAR_TARGET) && getf("MrtColorFill", 0.0f) > 0.5f) { HRESULT hrc = IDirect3DDevice9_ColorFill(d, g_mrt_surf, NULL, c); if (SUCCEEDED(hrc)) { f &= ~D3DCLEAR_TARGET; if (!f) return D3D_OK; } }
    if (g_mrt && !n && !g_in_post && getf("MrtClearRect", 0.0f) > 0.5f) { D3DRECT fr; fr.x1 = 0; fr.y1 = 0; fr.x2 = (LONG)g_mrt_px; fr.y2 = (LONG)g_mrt_px; return o_CLR(d, 1, &fr, f, c, z, s); }
    if (g_mrt && n && r && n <= 16 && !g_in_post) { D3DRECT sr[16]; DWORD i; for (i = 0; i < n; i++) { sr[i].x1 = (LONG)(r[i].x1 * cfg_mrts + 0.5f); sr[i].y1 = (LONG)(r[i].y1 * cfg_mrts + 0.5f); sr[i].x2 = (LONG)(r[i].x2 * cfg_mrts + 0.5f); sr[i].y2 = (LONG)(r[i].y2 * cfg_mrts + 0.5f); } return o_CLR(d, n, sr, f, c, z, s); }
    if (V.on && V.rt_bb && n && r && n <= 16 && !g_in_post) { D3DRECT sr[16]; DWORD i; for (i = 0; i < n; i++) { sr[i].x1 = r[i].x1 * V.pw / V.vw; sr[i].y1 = r[i].y1 * V.ph / V.vh; sr[i].x2 = r[i].x2 * V.pw / V.vw; sr[i].y2 = r[i].y2 * V.ph / V.vh; } return o_CLR(d, n, sr, f, c, z, s); }
    { HRESULT hrx = o_CLR(d, n, r, f, c, z, s); if (g_mrt && g_mrt_surf && g_mrt_passno == g_mrt_moviepass && !g_in_post) mrt_dump2(d, g_mrt_surf, "TheMoviesGfx.clrdump.now", "TheMoviesGfx.clrdump.bmp"); return hrx; }
}
static HRESULT WINAPI h_FVF(IDirect3DDevice9 *d, DWORD f) { V.fvf = f; if (PROBING()) plog("SetFVF 0x%lx", f); return o_FVF(d, f); }
static HRESULT WINAPI h_VD(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) { if (!g_in_post) V.fvf = 0; if (PROBING()) plog("SetVertexDeclaration %p", (void *)v); return o_VD(d, v); }
static HRESULT WINAPI h_VS(IDirect3DDevice9 *d, IDirect3DVertexShader9 *v) { if (PROBING()) plog("SetVertexShader %p", (void *)v); return o_VS(d, v); }
static HRESULT WINAPI h_SW(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *p, IDirect3DSwapChain9 **s) { logmsg("CreateAdditionalSwapChain %ux%u (game uses extra swap chains!)", p ? p->BackBufferWidth : 0, p ? p->BackBufferHeight : 0); return o_SW(d, p, s); }
static HRESULT WINAPI h_BEG(IDirect3DDevice9 *d) { if (PROBING()) plog("BeginScene"); return o_BEG(d); }
static HRESULT WINAPI h_END(IDirect3DDevice9 *d) { if (PROBING()) plog("EndScene"); return o_END(d); }
typedef HRESULT (WINAPI *PF_GVP)(IDirect3DDevice9 *, D3DVIEWPORT9 *);
static PF_GVP o_GVP;
static HRESULT WINAPI h_GVP(IDirect3DDevice9 *d, D3DVIEWPORT9 *v)
{
    HRESULT hr = o_GVP(d, v);
    if (g_mrt && cfg_dbg == 6 && SUCCEEDED(hr)) { static int nlv; if (nlv++ < 6) logmsg("MRT real viewport %lu,%lu %lux%lu z %.2f..%.2f", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ); }
    if (SUCCEEDED(hr) && v && g_mrt && !g_in_post) { v->X = (DWORD)(v->X / cfg_mrts + 0.5f); v->Y = (DWORD)(v->Y / cfg_mrts + 0.5f); v->Width = (DWORD)(v->Width / cfg_mrts + 0.5f); v->Height = (DWORD)(v->Height / cfg_mrts + 0.5f); return hr; }
    if (SUCCEEDED(hr) && v && V.on && V.rt_bb && !g_in_post) { v->X = v->X * V.vw / V.pw; v->Y = v->Y * V.vh / V.ph; v->Width = v->Width * V.vw / V.pw; v->Height = v->Height * V.vh / V.ph; }
    return hr;
}
typedef HRESULT (WINAPI *PF_CT)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *PF_CDS)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
static PF_CT o_CT; static PF_CDS o_CDS;
static HRESULT WINAPI h_TGLD(IDirect3DTexture9 *t, UINT l, D3DSURFACE_DESC *d)
{
    HRESULT hr = o_TGLD(t, l, d);
    if (SUCCEEDED(hr) && d && !g_in_post && d->Width == g_mrt_px && d->Height == g_mrt_px && (d->Usage & D3DUSAGE_RENDERTARGET)) d->Width = d->Height = 512;
    return hr;
}
static HRESULT WINAPI h_CT(IDirect3DDevice9 *dv, UINT w, UINT h, UINT lv, DWORD us, D3DFORMAT f, D3DPOOL pool, IDirect3DTexture9 **pp, HANDLE *sh)
{
    if (cfg_mrts > 1.0f && w == 512 && h == 512 && (us & D3DUSAGE_RENDERTARGET) && pool == D3DPOOL_DEFAULT) {
        HRESULT hr = o_CT(dv, g_mrt_px, g_mrt_px, lv, us, f, pool, pp, sh);
        logmsg("movie RT: CreateTexture 512x512 levels %u usage 0x%lx fmt %d -> %ux%u (hr %lx) caller %p", lv, us, (int)f, g_mrt_px, g_mrt_px, (unsigned long)hr, __builtin_return_address(0));
        { void **sp = (void **)__builtin_frame_address(0); int q; char tb[1200] = ""; for (q = 0; q < 400 && !IsBadReadPtr(sp, 4); q++, sp++) { unsigned long v = (unsigned long)*sp; if (v >= 0x401000 && v < 0xd16000 && !IsBadReadPtr((void *)(v - 6), 8) && (*(unsigned char *)(v - 5) == 0xe8 || *(unsigned char *)(v - 2) == 0xff || *(unsigned char *)(v - 6) == 0xff)) { char o[24]; snprintf(o, sizeof o, "%lx ", v); strcat(tb, o); if (strlen(tb) > 1100) break; } } logmsg("movie RT stack scan: %s", tb); }
        if (SUCCEEDED(hr) && pp && *pp) g_mrt_tex = (void *)*pp;
        if (SUCCEEDED(hr) && pp && *pp && !o_TGLD) { void *o = patch_vt(*pp, 17, h_TGLD); if (o) o_TGLD = (PF_TGLD)o; }
        return hr;
    }
    if ((us & D3DUSAGE_RENDERTARGET) && w >= 256 && w != 2560 && w != g_mr_res) { void **sp = (void **)__builtin_frame_address(0); int q; char tb[1200] = ""; for (q = 0; q < 600 && !IsBadReadPtr(sp, 4); q++, sp++) { unsigned long v = (unsigned long)*sp; if (v >= 0x401000 && v < 0xd16000 && !IsBadReadPtr((void *)(v - 6), 8) && (*(unsigned char *)(v - 5) == 0xe8 || *(unsigned char *)(v - 2) == 0xff || *(unsigned char *)(v - 6) == 0xff)) { char o[24]; snprintf(o, sizeof o, "%lx ", v); strcat(tb, o); if (strlen(tb) > 1100) break; } } logmsg("RT %ux%u created, call stack: %s", w, h, tb); }
    { HRESULT hrt = o_CT(dv, w, h, lv, us, f, pool, pp, sh); if ((us & D3DUSAGE_RENDERTARGET) && w >= 256) logmsg("CT render-target texture %ux%u levels %u fmt %d -> %lx caller %p", w, h, lv, (int)f, (unsigned long)hrt, __builtin_return_address(0)); else if (FAILED(hrt)) logmsg("CreateTexture %ux%u FAILED %lx", w, h, (unsigned long)hrt); return hrt; }
}
static HRESULT WINAPI h_CDS(IDirect3DDevice9 *dv, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL dis, IDirect3DSurface9 **pp, HANDLE *sh)
{
    { static int nlc; if (nlc++ < 30) logmsg("CreateDepthStencilSurface %ux%u fmt %d ms %d caller %p", w, h, (int)f, (int)ms, __builtin_return_address(0)); }
    if (cfg_mrts > 1.0f && w == 512 && h == 512) { HRESULT hr = o_CDS(dv, g_mrt_px, g_mrt_px, f, ms, q, dis, pp, sh); logmsg("movie RT: CreateDepthStencilSurface 512x512 -> %ux%u (hr %lx)", g_mrt_px, g_mrt_px, (unsigned long)hr); return hr; }
    return o_CDS(dv, w, h, f, ms, q, dis, pp, sh);
}
typedef HRESULT (WINAPI *PF_SR)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
typedef HRESULT (WINAPI *PF_CF)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, D3DCOLOR);
typedef HRESULT (WINAPI *PF_SDS)(IDirect3DDevice9 *, IDirect3DSurface9 *);
static PF_SR o_SR; static PF_CF o_CF; static PF_SDS o_SDS;
static void sd_str(IDirect3DSurface9 *s, char *o, size_t n) { D3DSURFACE_DESC dd; if (s && !IsBadReadPtr(s, 8) && SUCCEEDED(real_desc(s, &dd))) snprintf(o, n, "%ux%u/0x%lx", dd.Width, dd.Height, (unsigned long)dd.Usage); else snprintf(o, n, "null"); }
static HRESULT WINAPI h_SR(IDirect3DDevice9 *dv, IDirect3DSurface9 *a, const RECT *ra, IDirect3DSurface9 *b, const RECT *rb, D3DTEXTUREFILTERTYPE f)
{ mrt_seq("StretchRect"); D3DSURFACE_DESC dsa, dsb; int bigsr = (a && b && SUCCEEDED(real_desc(a, &dsa)) && SUCCEEDED(real_desc(b, &dsb)) && cfg_mrts > 1.0f && dsa.Width == g_mrt_px && dsb.Width == g_mrt_px); static int nl; if (cfg_dbg == 6 && !g_in_post && nl < 150) { char s1[40], s2[40]; sd_str(a, s1, sizeof s1); sd_str(b, s2, sizeof s2); if (strncmp(s1, "2560x1440", 9) || strncmp(s2, "2560x1440", 9)) { nl++; logmsg("StretchRect caller %p [%p%s -> %p%s pass %d frame %lu] %s %ld,%ld,%ld,%ld -> %s %ld,%ld,%ld,%ld", __builtin_return_address(0), (void *)a, a == g_mrt_surf ? "=MRT" : "", (void *)b, b == g_mrt_surf ? "=MRT" : "", g_mrt_passno, G.frame, s1, ra ? ra->left : -1, ra ? ra->top : -1, ra ? ra->right : -1, ra ? ra->bottom : -1, s2, rb ? rb->left : -1, rb ? rb->top : -1, rb ? rb->right : -1, rb ? rb->bottom : -1); } } int srwant = 0; if (bigsr && !g_in_post) { char tq[MAX_PATH]; strcpy(tq, g_logp); strcpy(strrchr(tq, (char)92) + 1, "TheMoviesGfx.srdump.now"); srwant = (GetFileAttributesA(tq) != INVALID_FILE_ATTRIBUTES); if (srwant) mrt_dump2(dv, a, "TheMoviesGfx.srdump.now", "TheMoviesGfx.sr_before.bmp"); } if (cfg_mrts > 1.0f && a && b && !ra && !rb && !g_in_post && getf("MrtDrawCopy", 0.0f) > 0.5f) { D3DSURFACE_DESC da, db; if (SUCCEEDED(real_desc(a, &da)) && SUCCEEDED(real_desc(b, &db)) && da.Width == g_mrt_px && db.Width == g_mrt_px && da.Height == da.Width && db.Height == db.Width && (da.Usage & D3DUSAGE_RENDERTARGET)) { IDirect3DTexture9 *srct = NULL; if (SUCCEEDED(IDirect3DSurface9_GetContainer(a, &g_iid_tex9, (void **)&srct)) && srct) { IDirect3DStateBlock9 *sb = NULL; IDirect3DSurface9 *oldrt = NULL; if (SUCCEEDED(IDirect3DDevice9_CreateStateBlock(dv, D3DSBT_ALL, &sb)) && sb) { struct { float x, y, z, rhw, u, v; } qv[4] = { {0, 0, 0, 1, 0, 0}, {(float)g_mrt_px, 0, 0, 1, 1, 0}, {(float)g_mrt_px, (float)g_mrt_px, 0, 1, 1, 1}, {0, (float)g_mrt_px, 0, 1, 0, 1} }; D3DVIEWPORT9 ovp, nvp; IDirect3DSurface9 *odss = NULL; IDirect3DStateBlock9_Capture(sb); IDirect3DDevice9_GetRenderTarget(dv, 0, &oldrt); IDirect3DDevice9_GetViewport(dv, &ovp); IDirect3DDevice9_GetDepthStencilSurface(dv, &odss); g_in_post = 1; o_RT(dv, 0, b); IDirect3DDevice9_SetDepthStencilSurface(dv, NULL); nvp.X = 0; nvp.Y = 0; nvp.Width = g_mrt_px; nvp.Height = g_mrt_px; nvp.MinZ = 0; nvp.MaxZ = 1; IDirect3DDevice9_SetViewport(dv, &nvp); IDirect3DDevice9_SetVertexShader(dv, NULL); IDirect3DDevice9_SetPixelShader(dv, NULL); IDirect3DDevice9_SetFVF(dv, D3DFVF_XYZRHW | D3DFVF_TEX1); IDirect3DDevice9_SetTexture(dv, 0, (IDirect3DBaseTexture9 *)srct); IDirect3DDevice9_SetRenderState(dv, D3DRS_ALPHABLENDENABLE, FALSE); IDirect3DDevice9_SetRenderState(dv, D3DRS_ZENABLE, FALSE); IDirect3DDevice9_SetRenderState(dv, D3DRS_STENCILENABLE, FALSE); IDirect3DDevice9_SetRenderState(dv, D3DRS_SCISSORTESTENABLE, FALSE); IDirect3DDevice9_SetRenderState(dv, D3DRS_CULLMODE, D3DCULL_NONE); IDirect3DDevice9_SetRenderState(dv, D3DRS_COLORWRITEENABLE, 15); IDirect3DDevice9_SetTextureStageState(dv, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); IDirect3DDevice9_SetTextureStageState(dv, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE); IDirect3DDevice9_SetTextureStageState(dv, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); IDirect3DDevice9_SetTextureStageState(dv, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); IDirect3DDevice9_SetTextureStageState(dv, 1, D3DTSS_COLOROP, D3DTOP_DISABLE); IDirect3DDevice9_SetSamplerState(dv, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); IDirect3DDevice9_SetSamplerState(dv, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); IDirect3DDevice9_SetSamplerState(dv, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); IDirect3DDevice9_SetSamplerState(dv, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); o_DPUP(dv, D3DPT_TRIANGLEFAN, 2, qv, sizeof qv[0]); IDirect3DStateBlock9_Apply(sb); if (oldrt) { o_RT(dv, 0, oldrt); IDirect3DSurface9_Release(oldrt); } IDirect3DDevice9_SetViewport(dv, &ovp); IDirect3DDevice9_SetDepthStencilSurface(dv, odss); if (odss) IDirect3DSurface9_Release(odss); g_in_post = 0; IDirect3DStateBlock9_Release(sb); IDirect3DTexture9_Release(srct); return D3D_OK; } IDirect3DTexture9_Release(srct); } } }
    { HRESULT hrs = o_SR(dv, a, ra, b, rb, f); { static int nlsr; if (FAILED(hrs) && nlsr++ < 20) logmsg("StretchRect FAILED %lx", (unsigned long)hrs); } if (srwant) { char tp[MAX_PATH]; strcpy(tp, g_logp); strcpy(strrchr(tp, (char)92) + 1, "TheMoviesGfx.srdump.keep"); { FILE *kf = fopen(tp, "w"); if (kf) fclose(kf); } mrt_dump2(dv, a, "TheMoviesGfx.srdump.keep", "TheMoviesGfx.sr_after.bmp"); { FILE *kf2 = fopen(tp, "w"); if (kf2) fclose(kf2); } mrt_dump2(dv, b, "TheMoviesGfx.srdump.keep", "TheMoviesGfx.sr_dst.bmp"); } return hrs; } }
static HRESULT WINAPI h_CF(IDirect3DDevice9 *dv, IDirect3DSurface9 *a, const RECT *r, D3DCOLOR c)
{ mrt_seq("ColorFill"); static int nl; if (cfg_dbg == 6 && nl++ < 80) { char s1[40]; sd_str(a, s1, sizeof s1); logmsg("ColorFill %s %ld,%ld,%ld,%ld col %08lx", s1, r ? r->left : -1, r ? r->top : -1, r ? r->right : -1, r ? r->bottom : -1, (unsigned long)c); } return o_CF(dv, a, r, c); }
static HRESULT WINAPI h_SDS(IDirect3DDevice9 *dv, IDirect3DSurface9 *a)
{ { static int nls; if (nls++ < 60) { char s1[40]; sd_str(a, s1, sizeof s1); logmsg("SetDepthStencilSurface %s caller %p", s1, __builtin_return_address(0)); } } mrt_seq("SetDepthStencil"); static int nl; if (cfg_dbg == 6 && nl++ < 80) { char s1[40]; sd_str(a, s1, sizeof s1); logmsg("SetDepthStencilSurface %s (mrt %d)", s1, g_mrt); } return o_SDS(dv, a); }
typedef HRESULT (WINAPI *PF_CRT)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *);
static PF_CRT o_CRT;
static HRESULT WINAPI h_CRT(IDirect3DDevice9 *dv, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lk, IDirect3DSurface9 **pp, HANDLE *sh)
{
    static int nl; if (nl++ < 40) logmsg("CreateRenderTarget %ux%u fmt %d ms %d lockable %d caller %p", w, h, (int)f, (int)ms, (int)lk, __builtin_return_address(0));
    if (nl <= 3) { void **fp = (void **)__builtin_frame_address(1); int q; char tb[400] = ""; for (q = 0; q < 10 && fp && !IsBadReadPtr(fp, 8); q++) { char o[24]; snprintf(o, sizeof o, "%p ", fp[1]); strcat(tb, o); void **nx = (void **)fp[0]; if (nx <= fp || (char *)nx - (char *)fp > 0x10000) break; fp = nx; } logmsg("CRT stack (return addresses): %s", tb); }
    if (cfg_mrts > 1.0f && w == 512 && h == 512) { HRESULT hr = o_CRT(dv, g_mrt_px, g_mrt_px, f, ms, q, lk, pp, sh); logmsg("movie RT: CreateRenderTarget 512x512 -> %ux%u (hr %lx)", g_mrt_px, g_mrt_px, (unsigned long)hr); return hr; }
    { HRESULT hrc = o_CRT(dv, w, h, f, ms, q, lk, pp, sh); if (FAILED(hrc)) logmsg("CreateRenderTarget %ux%u FAILED %lx caller %p", w, h, (unsigned long)hrc, __builtin_return_address(0)); return hrc; }
}
static void probe_hook_device(IDirect3DDevice9 *dev)
{
    void *o;
#define HK(idx, fn, store, type) do { if ((o = patch_vt(dev, idx, fn))) store = (type)o; } while (0)
    HK(81, h_DP, o_DP, PF_DP); HK(82, h_DIP, o_DIP, PF_DIP); HK(83, h_DPUP, o_DPUP, PF_DPUP); HK(84, h_DIPUP, o_DIPUP, PF_DIPUP);
    HK(48, h_GVP, o_GVP, PF_GVP); HK(47, h_VP, o_VP, PF_VP); HK(75, h_SC, o_SC, PF_SC); HK(37, h_RT, o_RT, PF_RT); HK(44, h_TR, o_TR, PF_TR); HK(43, h_CLR, o_CLR, PF_CLR);
    if (!g_dbg_nohooks) { HK(28, h_CRT, o_CRT, PF_CRT); HK(34, h_SR, o_SR, PF_SR); HK(35, h_CF, o_CF, PF_CF); HK(39, h_SDS, o_SDS, PF_SDS);
    HK(23, h_CT, o_CT, PF_CT); HK(29, h_CDS, o_CDS, PF_CDS); }
    HK(89, h_FVF, o_FVF, PF_FVF); HK(87, h_VD, o_VD, PF_VD); HK(92, h_VS, o_VS, PF_VS); HK(13, h_SW, o_SW, PF_SW); HK(41, h_BEG, o_BEG, PF_SC0); HK(42, h_END, o_END, PF_SC0);
#undef HK
}
static void probe_begin(void)
{
    char p[MAX_PATH]; strcpy(p, g_logp); strcpy(strrchr(p, '\\') + 1, "TheMoviesGfx.probe.log");
    g_pf = fopen(p, "a"); g_pn = 0; g_probe_left = 2;
    plog("==== probe capture (frame %lu) ====", G.frame);
    logmsg("probe capture started");
}
static void probe_end(void) { if (g_pf) { plog("==== end (%u draw calls logged) ====", g_pn); fclose(g_pf); g_pf = NULL; } logmsg("probe capture finished"); }


/* ---------------- memory scan (reverse engineering aid): find the game's UI scale floats ---------------- */
static void mem_scan(float sx, float sy)
{
    SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi; BYTE *a; DWORD fx, fy; int found = 0; HMODULE exe = GetModuleHandleA(NULL);
    memcpy(&fx, &sx, 4); memcpy(&fy, &sy, 4);
    GetSystemInfo(&si);
    logmsg("memory scan for sx=%.4f (0x%08lx) near sy=%.4f (0x%08lx)", sx, fx, sy, fy);
    for (a = (BYTE *)si.lpMinimumApplicationAddress; a < (BYTE *)si.lpMaximumApplicationAddress && found < 300; a += mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD) && mbi.RegionSize < 0x20000000) {
            BYTE *p = (BYTE *)mbi.BaseAddress, *end = p + mbi.RegionSize - 4;
            for (; p <= end; p += 4) {
                if (*(DWORD *)p == fx) {
                    int k; for (k = -32; k <= 32; k++) { BYTE *q = p + k * 4; if (q >= (BYTE *)mbi.BaseAddress && q <= end && q != p && *(DWORD *)q == fy) {
                        logmsg("  hit sx@%p sy@%p (delta %d dwords) region %p size %lx type %s%s", (void *)p, (void *)q, k, mbi.BaseAddress, (unsigned long)mbi.RegionSize,
                               mbi.Type == MEM_IMAGE ? "IMAGE" : mbi.Type == MEM_PRIVATE ? "PRIVATE" : "MAPPED", ((BYTE *)mbi.BaseAddress >= (BYTE *)exe && (BYTE *)mbi.BaseAddress < (BYTE *)exe + 0x1100000) ? " (game exe)" : "");
                        found++; break; } }
                }
            }
        }
    }
    logmsg("memory scan done, %d candidate pairs", found);
}



static void mem_scan_one(float v, const char *label)
{
    SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi; BYTE *a; DWORD fv; int found = 0; BYTE *lo = (BYTE *)0x400000, *hi = (BYTE *)0x1200000; /* the game exe image */
    memcpy(&fv, &v, 4); GetSystemInfo(&si);
    logmsg("scan %s: float %.4f (0x%08lx) in the game image (writable data only)", label, v, fv);
    for (a = lo; a < hi && found < 200; a += mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
            BYTE *p = (BYTE *)mbi.BaseAddress, *end = p + mbi.RegionSize - 4;
            for (; p <= end; p += 4) if (*(DWORD *)p == fv) { logmsg("  %p", (void *)p); found++; }
        }
    }
    logmsg("scan %s done: %d hits", label, found);
}



/* ---------------- cursor finder: locate the game's own cursor position variable ---------------- */
/* Trigger file TheMoviesGfx.curscan.now. Each trigger takes the cursor sprite's current raw position (g_cur_x/g_cur_y) and keeps the (addrX, k) pairs where
 * dword@addrX and dword@addrX+4k both hold values within +-6 of x and y (as int or float). The first trigger scans memory, later ones filter the survivors. */
#define CS_MAX 40000
static struct { DWORD *a; int k; } g_cs[CS_MAX]; static int g_ncs, g_csinit;
static int cs_match(DWORD v, float t)
{
    int iv = (int)v; float fv; memcpy(&fv, &v, 4);
    if (iv >= 0 && iv < 100000 && iv >= (int)t - 6 && iv <= (int)t + 6) return 1;
    if (fv == fv && fv > t - 6.0f && fv < t + 6.0f && fv > 1.0f) return 1;
    return 0;
}
static void cursor_scan(void)
{
    float x = g_cur_x, y = g_cur_y; SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi; BYTE *a; int i, j = 0;
    if (!g_cur_ok || x < 40 || y < 40) { logmsg("curscan: no usable cursor position (%.0f,%.0f)", x, y); return; }
    logmsg("curscan: cursor quad at (%.0f,%.0f), %s", x, y, g_csinit ? "filtering survivors" : "scanning memory");
    if (g_csinit) {
        for (i = 0; i < g_ncs; i++) { DWORD *p = g_cs[i].a; if (cs_match(p[0], x) && cs_match(p[g_cs[i].k], y)) g_cs[j++] = g_cs[i]; }
        g_ncs = j;
    } else {
        GetSystemInfo(&si); g_ncs = 0; g_csinit = 1;
        for (a = (BYTE *)si.lpMinimumApplicationAddress; a < (BYTE *)si.lpMaximumApplicationAddress && g_ncs < CS_MAX; a += mbi.RegionSize) {
            if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) && mbi.RegionSize < 0x10000000) {
                DWORD *p = (DWORD *)mbi.BaseAddress, *end = (DWORD *)((BYTE *)mbi.BaseAddress + mbi.RegionSize) - 1; int k;
                for (; p <= end && g_ncs < CS_MAX; p++) if (cs_match(*p, x)) for (k = -4; k <= 4; k++) { DWORD *q = p + k; if (k && q >= (DWORD *)mbi.BaseAddress && q <= end && cs_match(*q, y)) { g_cs[g_ncs].a = p; g_cs[g_ncs].k = k; g_ncs++; break; } }
            }
        }
    }
    logmsg("curscan: %d candidates", g_ncs);
    for (i = 0; i < g_ncs && i < 40; i++) { DWORD *p = g_cs[i].a; float fx, fy; memcpy(&fx, &p[0], 4); memcpy(&fy, &p[g_cs[i].k], 4); logmsg("  X@%p Y@%p (k=%d) ints (%d,%d) floats (%.1f,%.1f)", (void *)p, (void *)(p + g_cs[i].k), g_cs[i].k, (int)p[0], (int)p[g_cs[i].k], fx, fy); }
}


/* ---------------- hardware write watchpoint (reverse engineering aid): who writes this address? ---------------- */
/* Trigger file TheMoviesGfx.watch.now containing "hexaddr [hexaddr2]". Uses DR0/DR1 (write, 4 bytes) on the thread that presents frames, logs EIP + registers of each writer. */
static volatile unsigned long g_watch_addr[2]; static unsigned g_nwatch; static DWORD g_watch_tid; static int g_watch_rw; static unsigned long g_seen_eip[256]; static int g_nseen;
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 3)) {
        CONTEXT *c = ep->ContextRecord; BYTE *ip = (BYTE *)c->Eip; char hex[64] = ""; int i;
        for (i = -12; i < 8; i++) { char t[4]; sprintf(t, "%02x", ip[i]); strcat(hex, t); if (i == -1) strcat(hex, "|"); }
        if (g_watch_rw) { int k, seen = 0; for (k = 0; k < g_nseen; k++) if (g_seen_eip[k] == c->Eip) seen = 1; if (seen) { c->Dr6 = 0; c->EFlags |= 0x10000; return EXCEPTION_CONTINUE_EXECUTION; } if (g_nseen < 256) g_seen_eip[g_nseen++] = c->Eip; }
        if (g_nwatch < 200) { char cs[200] = ""; int k, nc = 0; const unsigned long *sp = (const unsigned long *)c->Esp; for (k = 0; k < 64 && nc < 8; k++) if (!IsBadReadPtr(sp + k, 4) && sp[k] >= 0x401000 && sp[k] < 0xd15000) { char t2[16]; sprintf(t2, " %lx", sp[k]); strcat(cs, t2); nc++; } logmsg("watch stack:%s", cs); }
        if (g_nwatch++ < 200) logmsg("watch hit dr6=%lx EIP=%08lx bytes[-12..+8]=%s eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx ret=%08lx", c->Dr6, c->Eip, hex, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, *(DWORD *)c->Esp);
        c->Dr6 = 0; c->EFlags |= 0x10000; return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI watch_thread(LPVOID p)
{
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, g_watch_tid); CONTEXT c;
    if (!th) { logmsg("watch: OpenThread failed"); return 0; }
    SuspendThread(th); memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(th, &c);
    c.Dr0 = g_watch_addr[0]; c.Dr1 = g_watch_addr[1]; c.Dr6 = 0;
    { unsigned rw = g_watch_rw ? 3u : 1u; /* RW=01 write, 11 read/write */
    c.Dr7 = 0; if (g_watch_addr[0]) c.Dr7 |= (1u << 0) | (rw << 16) | (3u << 18); /* DR0: local enable, LEN=11 (4 bytes) */
    if (g_watch_addr[1]) c.Dr7 |= (1u << 2) | (rw << 20) | (3u << 22); }
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS; logmsg("watch: SetThreadContext -> %d (Dr0=%08lx Dr1=%08lx Dr7=%08lx)", SetThreadContext(th, &c), c.Dr0, c.Dr1, c.Dr7);
    ResumeThread(th); CloseHandle(th); return 0;
}
static void watch_start(unsigned long a0, unsigned long a1)
{
    g_nseen = 0;
    static int veh; g_watch_addr[0] = a0; g_watch_addr[1] = a1; g_watch_tid = GetCurrentThreadId(); g_nwatch = 0;
    if (!veh) { AddVectoredExceptionHandler(1, watch_veh); veh = 1; }
    CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
}


/* ---------------- widget finder: enumerate widget-like heap objects whose laid-out rect lies in the top-left block ---------------- */
/* widget layout (found by reverse engineering): edge records of 0x24 bytes at +0x98 (top) +0xbc (left) +0xe0 (bottom) +0x104 (right): {float input, float absolute px, int type, ...} */
static void widget_dump(void)
{
    SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi; BYTE *a; int n = 0;
    GetSystemInfo(&si); logmsg("widget dump: scanning heap for objects with a vtable pointer and a plausible layout rect");
    for (a = (BYTE *)si.lpMinimumApplicationAddress; a < (BYTE *)si.lpMaximumApplicationAddress && n < 400; a += mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) && mbi.Type == MEM_PRIVATE && mbi.RegionSize < 0x10000000) {
            DWORD *p = (DWORD *)mbi.BaseAddress, *end = (DWORD *)((BYTE *)mbi.BaseAddress + mbi.RegionSize) - 0x50;
            for (; p < end && n < 400; p++) {
                float t, l, b, r; int i;
                if (p[0] < 0x00D30000 || p[0] >= 0x00D60000) continue;
                t = *(float *)((BYTE *)p + 0x9c); l = *(float *)((BYTE *)p + 0xc0); b = *(float *)((BYTE *)p + 0xe4); r = *(float *)((BYTE *)p + 0x108);
                if (!(l >= 0 && r > l && r <= 400 && t >= 0 && b > t && b <= 140 && r - l >= 4 && b - t >= 4)) continue;
                logmsg("W@%p vt=%08lx rect L%.0f T%.0f R%.0f B%.0f", (void *)p, p[0], l, t, r, b); n++;
                for (i = 0; i < 4; i++) { DWORD *e = (DWORD *)((BYTE *)p + 0x98 + 0x24 * i); float f0, f1; memcpy(&f0, &e[0], 4); memcpy(&f1, &e[1], 4);
                    logmsg("    rec%d@+%x: f0=%.3f f1=%.3f w2=%08lx w3=%08lx w4=%08lx w5=%08lx w6=%08lx w7=%08lx w8=%08lx", i, 0x98 + 0x24 * i, f0, f1, e[2], e[3], e[4], e[5], e[6], e[7], e[8]); }
            }
        }
    }
    logmsg("widget dump done: %d objects", n);
}


/* ---------------- widget layout scaling (the real fix for the time controls) ---------------- */
/* Each widget keeps its four edge INPUTS as floats at +0x98 (top) +0xbc (left) +0xe0 (bottom=height or offset) +0x104 (right=width or offset); the laid-out px values at +0x9c/+0xc0/+0xe4/+0x108
 * are recomputed from them. Scaling the inputs makes the game draw AND hit-test the bigger layout. The HUD uses fixed px at every resolution, so the originals can be matched exactly. */
typedef struct { DWORD vt; float f[4]; float abs_[4]; /* top, left, bottom, right (unscaled laid-out px) */ int rel_top; /* bit0: top is relative to the previous sibling (scale it plainly); bit1: the game re-sets the left input to a value of its own (accept any) */ } WFp;
static const WFp g_wfp[] = {
    { 0x00d39ae4, { 16, 112, 32, 132 }, { 16, 112, 48, 244 }, 0 },  /* button strip background */
    { 0x00d39ae4, { 2, 0, 64, 256 },    { 2, 0, 66, 256 }, 0 },     /* date pill */
    { 0x00d39ae4, { 16, 0, 64, 256 },   { 16, 0, 80, 256 }, 0 },    /* pill lower part */
    { 0x00d39d5c, { 14, 144, 24, 24 },  { 14, 144, 38, 168 }, 2 },  /* button 1 (reset); the game re-sets its left to 204 */
    { 0x00d39d5c, { 14, 176, 24, 24 },  { 14, 176, 38, 200 }, 0 },  /* button 2 (pause) */
    { 0x00d39d5c, { 0, 4, 24, 24 },     { 14, 196, 38, 220 }, 1 },  /* button 3 (play), chained to the previous one */
    { 0x00d39d5c, { 0, 4, 24, 24 },     { 14, 216, 38, 240 }, 1 },  /* button 4 (fast forward) */
    { 0x00d5e54c, { 20, 14, 24, 120 },  { 20, 14, 44, 134 }, 0 },   /* date text */
};
#define NWFP ((int)(sizeof g_wfp / sizeof g_wfp[0]))
static BYTE *volatile g_mb; static volatile float g_mb_left, g_mb_top; /* menu button override, see install_menu_btn_patch */
static BYTE *g_wptr[NWFP]; static float g_wapplied = 1.0f; static volatile LONG g_wscan_busy; static DWORD g_wscan_tick;
static float w_target(const WFp *fp, int i, float z)
{
    if (i == 0 && !(fp->rel_top & 1)) return BAR_BOTTOM * cfg_bar + (fp->f[0] - BAR_BOTTOM) * z;
    return fp->f[i] * z;
}
/* every input must equal its original or its currently applied value (the game re-sets some of them, e.g. a button's position, which leaves a mixture) */
static int w_matches(const BYTE *w, const WFp *fp, float z_prev, int strict_abs)
{
    static const int fo[4] = { 0x98, 0xbc, 0xe0, 0x104 }, ao[4] = { 0x9c, 0xc0, 0xe4, 0x108 }; int i; int all_orig = 1;
    if (*(const DWORD *)w != fp->vt) return 0;
    for (i = 0; i < 4; i++) {
        float f = *(const float *)(w + fo[i]), o = fp->f[i], sc = w_target(fp, i, z_prev);
        if (i == 1 && (fp->rel_top & 2) && f > 190.0f && f < 240.0f) { if (fabsf(f - o) > 0.01f) all_orig = 0; continue; }
        if (fabsf(f - o) <= 0.01f) continue; all_orig = 0;
        if (fabsf(f - sc) > 0.01f) return 0;
    }
    if (all_orig || strict_abs) for (i = 0; i < 4; i++) if (all_orig && fabsf(*(const float *)(w + ao[i]) - fp->abs_[i]) > 0.5f) return 0;
    if (!all_orig) { float al = *(const float *)(w + 0xc0), at = *(const float *)(w + 0x9c); if (!(al >= 0 && al <= 600 && at >= -40 && at <= 160)) return 0; }
    return 1;
}
#define SCAN_CHUNK (128 * 1024)
static DWORD WINAPI widget_scan_thread(LPVOID unused)
{
    SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi; BYTE *a; int i, nc3 = 0; BYTE *c3[32], *c3par[32]; float zp = g_wapplied; BYTE *buf = (BYTE *)malloc(SCAN_CHUNK + 0x120); HANDLE me = GetCurrentProcess();
    if (!buf) { InterlockedExchange(&g_wscan_busy, 0); return 0; }
    GetSystemInfo(&si);
    for (a = (BYTE *)si.lpMinimumApplicationAddress; a < (BYTE *)si.lpMaximumApplicationAddress; a += mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) && mbi.Type == MEM_PRIVATE && mbi.RegionSize < 0x10000000) {
            SIZE_T off, rsz = mbi.RegionSize;
            for (off = 0; off + 0x110 < rsz; off += SCAN_CHUNK) { /* copy with ReadProcessMemory: a page the game frees meanwhile makes the copy fail instead of crashing the process */
                SIZE_T n = rsz - off < SCAN_CHUNK + 0x110 ? rsz - off : SCAN_CHUNK + 0x110, got = 0, lim, ci;
                if (!ReadProcessMemory(me, (BYTE *)mbi.BaseAddress + off, buf, n, &got) || got < 0x120) continue;
                lim = got - 0x110; if (lim > SCAN_CHUNK) lim = SCAN_CHUNK;
                for (ci = 0; ci < lim; ci += 4) {
                    BYTE *cp = buf + ci, *p = (BYTE *)mbi.BaseAddress + off + ci; DWORD vt = *(DWORD *)cp; int known = 0;
                    if (vt == 0x00d3358c && fabsf(*(float *)(cp + 0xbc) - 212.0f) < 0.5f && fabsf(*(float *)(cp + 0x104) - 600.0f) < 0.5f) g_page_root = p; /* Create Your Studio panel */
                    if (vt == 0x00d32c7c || vt == 0x00d4a364) { /* money pill / studio rating: remember the instance (stale copies are filtered in widgets_apply by the rating -> money child link) */
                        if (vt == 0x00d32c7c && !g_money_w) g_money_w = p; else if (vt == 0x00d4a364 && !g_rate_w) g_rate_w = p;
                        continue; }
                    if (vt != 0x00d39ae4 && vt != 0x00d39d5c && vt != 0x00d5e54c) continue;
                    for (i = 0; i < NWFP; i++) if (g_wptr[i] == p) known = 1;
                    if (known) continue;
                    for (i = 0; i < NWFP; i++) { if (g_wptr[i] || !w_matches(cp, &g_wfp[i], zp, 0)) continue;
                        if (i == 3) { if (nc3 < 32) { c3[nc3] = p; c3par[nc3++] = *(BYTE **)(cp + 0xb8); } continue; } /* the lenient rule also matches stale copies in freed memory: decide after the scan */
                        g_wptr[i] = p; break; }
                }
            }
        }
    }
    free(buf);
    if (!g_wptr[3] && nc3) { /* the live menu button shares its parent with the pause button */
        BYTE *pp = g_wptr[4] && !IsBadReadPtr(g_wptr[4], 0x110) ? *(BYTE **)(g_wptr[4] + 0xb8) : NULL; int k, pick = -1;
        for (k = 0; k < nc3; k++) if (pp && c3par[k] == pp) { pick = k; break; }
        if (pick >= 0) { g_wptr[3] = c3[pick]; logmsg("menu button: %d candidates, picked %p (parent match)", nc3, (void *)c3[pick]); } else logmsg("menu button: %d candidates, no parent match", nc3);
    }
    { int n = 0; char miss[64] = ""; for (i = 0; i < NWFP; i++) { n += g_wptr[i] != NULL; if (!g_wptr[i]) { char t[8]; sprintf(t, "%d ", i); strcat(miss, t); } } logmsg("widget scan: %d/%d time-control widgets known (missing: %s) tick %lu frame %lu", n, NWFP, miss, (unsigned long)GetTickCount(), G.frame); }
    InterlockedExchange(&g_wscan_busy, 0); return 0;
}


/* full widget index: every vtable-bearing heap object with a plausible laid-out rect, written as CSV next to the dll (trigger file TheMoviesGfx.widgets.all) */
static void widget_dump_all(void)
{
    SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi; BYTE *a; int n = 0; char path[MAX_PATH]; FILE *f;
    strcpy(path, g_logp); strcpy(strrchr(path, '\\') + 1, "TheMoviesGfx.widgets.csv"); f = fopen(path, "w"); if (!f) return;
    fprintf(f, "addr,vtable,in_top,in_left,in_bottom,in_right,abs_top,abs_left,abs_bottom,abs_right,type_top,type_left,type_bottom,type_right,parent\n");
    GetSystemInfo(&si);
    for (a = (BYTE *)si.lpMinimumApplicationAddress; a < (BYTE *)si.lpMaximumApplicationAddress && n < 20000; a += mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) && mbi.Type == MEM_PRIVATE && mbi.RegionSize < 0x10000000) {
            BYTE *p = (BYTE *)mbi.BaseAddress, *end = p + mbi.RegionSize - 0x110;
            for (; p < end && n < 20000; p += 4) {
                DWORD vt = *(DWORD *)p; float it, il, ib, ir, t, l, b, r;
                if (vt < 0x00d00000 || vt >= 0x00e00000) continue;
                it = *(float *)(p + 0x98); il = *(float *)(p + 0xbc); ib = *(float *)(p + 0xe0); ir = *(float *)(p + 0x104);
                t = *(float *)(p + 0x9c); l = *(float *)(p + 0xc0); b = *(float *)(p + 0xe4); r = *(float *)(p + 0x108);
                if (!(l >= -400 && r > l && r <= 5000 && t >= -400 && b > t && b <= 3000 && r - l >= 2 && b - t >= 2)) continue;
                if (!(fabsf(it) < 5000 && fabsf(il) < 5000 && fabsf(ib) < 5000 && fabsf(ir) < 5000)) continue;
                fprintf(f, "%p,%08lx,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%lu,%lu,%08lx\n", (void *)p, vt, it, il, ib, ir, t, l, b, r,
                        *(DWORD *)(p + 0xa0), *(DWORD *)(p + 0xc4), *(DWORD *)(p + 0xe8), *(DWORD *)(p + 0x10c), *(DWORD *)(p + 0xb8)); n++;
            }
        }
    }
    fclose(f); logmsg("widget index: %d widgets written to TheMoviesGfx.widgets.csv", n);
}


/* ---------------- dialog scaling (options screens etc.) ---------------- */
/* A dialog is a panel widget (vtable 0xd3358c) with widgets inside its laid-out rect. Every input float is a px length (offset or size), so scaling them all by Z about the
 * panel's top-left scales the dialog; the panel's own top/left are adjusted so its centre stays put. Text glyphs (not layout driven) are enlarged about their text widget's centre. */
#define MAXDW 300
typedef struct { BYTE *w; DWORD vt; float f0[4]; int kind; /* 1 root, 2 child, 3 text child */ BYTE *root; } DW;
static DW g_dw[MAXDW]; static int g_ndw; static volatile LONG g_dscan_busy, g_dlock; static DWORD g_dscan_tick; static float g_dapplied = 1.0f;
/* Movie-Maker text boxes. The game draws glyphs 1.33x taller than the 1024x768 layout boxes of its text widgets (vt 0xd5e54c) and clips the glyph quads to the widget rectangle (abs T +0x9c, B +0xe4, copy at +0x1f4; input height +0xe0): the title, selector labels and list rows lose their descenders / second lines. While a Movie-Maker frame is up (g_mk_arrow + g_mk_bd) every single-line text widget in height mode gets its height x MakerTextBox (ini, default 1.34, 1 = off); the originals are restored when the maker is left. */
#define MAXTB 384
static struct { BYTE *w; float h0; int m; } g_tb[MAXTB]; static int g_ntb; static volatile LONG g_tb_busy, g_tb_lock; static int g_tb_on;
typedef void (__attribute__((thiscall)) *WSETH)(void *, float);

/* Construction-time fix: the text widget caches its glyph quads (clipped to its rect) when it is built, so the rect has to be big enough from the start. Height setter 0x73f490 (thiscall(widget, float)) and bottom-edge setter (edge_log) are patched for text widgets built while a Movie-Maker frame is up. */
float __cdecl __attribute__((used)) tb_adjust(void *self, float h)
{
    if (getf("MakerTextHook", 0.0f) > 0.5f && cfg_mktbox != 1.0f && G.frame - g_mk_arrow <= 8 && G.frame - g_mk_bd <= 8 && !IsBadReadPtr(self, 4) && *(DWORD *)self == 0x00d5e54c && h > 4.0f && h <= 90.0f) return h * cfg_mktbox;
    return h;
}

static float g_tx0[MAXDW], g_tx1[MAXDW], g_px0[MAXDW], g_px1[MAXDW]; static unsigned long g_tframe;
/* ---- Movie Player HUD: its widgets (a tree under one root) are scaled by rewriting their layout inputs, like the dialogs ---- */
#define MAXPW 220
typedef struct { BYTE *w; DWORD vt; float f0[4]; float t0; int mode; BYTE *par; float cur_top, last_e; int sgn; float az; int bz; float tb0; /* t0: laid-out top at scan; mode 1 = bottom-zone widget whose top hangs from a fixed base (band/screen): re-targeted about the bottom edge */ } PW;
static int g_pl_exp, g_pl_bot = 1; static PW g_pw[MAXPW]; static int g_npw; static BYTE *g_pl_root; static volatile LONG g_pscan_busy, g_plock; static DWORD g_pscan_tick; static float g_papplied = 1.0f;



/* ---------------- resolution list: also allow 16:9 modes ---------------- */
/* The game filters the display modes it offers in Video Options through bool allowed(int w, int h) at 0x9a53c0 (cdecl), which only accepts 800x600, 1024x768, 1152x864,
 * 1280x960, 1280x1024 and 1600x1200. We replace it with a version that also accepts 16:9 modes from 1280x720 up. */
static unsigned char __cdecl hk_mode_ok(int w, int h)
{
    if ((w == 800 && h == 600) || (w == 1024 && h == 768) || (w == 1152 && h == 864) || (w == 1280 && (h == 960 || h == 1024)) || (w == 1600 && h == 1200)) return 1;
    if (w >= 1280 && w <= 7680 && w * 9 == h * 16) return 1;
    return 0;
}
static void install_mode_patch(void)
{
    static int done; BYTE *f = (BYTE *)0x009a53c0; static const BYTE sig[] = { 0x8b, 0x4c, 0x24, 0x04, 0x32, 0xc0, 0x81, 0xf9, 0x20, 0x03, 0x00, 0x00 }; DWORD old; LONG rel;
    if (done) return; done = 1;
    if (IsBadReadPtr(f, 16) || memcmp(f, sig, sizeof sig)) { logmsg("mode list patch: code at 0x9a53c0 does not match, not patched"); return; }
    if (!VirtualProtect(f, 8, PAGE_EXECUTE_READWRITE, &old)) { logmsg("mode list patch: VirtualProtect failed"); return; }
    rel = (LONG)((BYTE *)hk_mode_ok - (f + 5)); f[0] = 0xe9; memcpy(f + 1, &rel, 4); VirtualProtect(f, 8, old, &old); FlushInstructionCache(GetCurrentProcess(), f, 8);
    logmsg("mode list patch installed: 16:9 modes (1280x720 ... 7680x4320) are now accepted by the game's resolution filter");
}

static void mem_poke(unsigned long addr, float v)
{
    DWORD old, d; MEMORY_BASIC_INFORMATION mbi; memcpy(&d, &v, 4);
    if (!VirtualQuery((void *)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) { logmsg("poke %08lx: unreadable", addr); return; }
    if (VirtualProtect((void *)addr, 4, PAGE_READWRITE, &old)) { *(DWORD *)addr = d; VirtualProtect((void *)addr, 4, old, &old); logmsg("poked %08lx = %.6f (0x%08lx)", addr, v, (unsigned long)d); }
    else logmsg("poke %08lx: VirtualProtect failed", addr);
}

static void mem_peek(unsigned long addr, int count)
{
    int i; logmsg("peek %08lx x%d", addr, count);
    for (i = 0; i < count; i++) {
        const BYTE *p = (const BYTE *)(addr + 4 * i); MEMORY_BASIC_INFORMATION mbi; float f; DWORD d;
        if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) { logmsg("  %08lx: unreadable", addr + 4 * i); continue; }
        d = *(const DWORD *)p; memcpy(&f, &d, 4);
        logmsg("  %08lx: 0x%08lx  float %.6f  int %ld", addr + 4 * i, (unsigned long)d, f, (long)d);
    }
}


/* ---------------- writer trace: which game code writes UI vertices? (reverse engineering aid) ---------------- */
typedef HRESULT (WINAPI *PF_VBLOCK)(IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD);
static PF_VBLOCK o_VBLock; static int g_trace_on; static int g_trace_locks; static PVOID g_veh;
static struct { unsigned long eip; unsigned long caller[6]; unsigned n; } g_wr[64]; static int g_nwr;
static LONG CALLBACK trace_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION && g_trace_on) {
        unsigned long eip = (unsigned long)ep->ExceptionRecord->ExceptionAddress; int i, found = -1;
        for (i = 0; i < g_nwr; i++) if (g_wr[i].eip == eip) { found = i; break; }
        if (found < 0 && g_nwr < 64) {
            const unsigned long *sp = (const unsigned long *)ep->ContextRecord->Esp; int k, c = 0;
            found = g_nwr++; g_wr[found].eip = eip;
            for (k = 0; k < 96 && c < 6; k++) { unsigned long v = sp[k]; if (v >= 0x401000 && v < 0xd15000) g_wr[found].caller[c++] = v; }
        }
        if (found >= 0) g_wr[found].n++;
        return EXCEPTION_CONTINUE_EXECUTION; /* the guard flag is cleared by the system; one hit per Lock is enough */
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
typedef HRESULT (WINAPI *PF_VBUNLOCK)(IDirect3DVertexBuffer9 *);
static PF_VBUNLOCK o_VBUnlock;
static struct { IDirect3DVertexBuffer9 *vb; BYTE *real, *shadow; UINT size; } g_sh[32]; static int g_nsh;
static HRESULT WINAPI h_VBLock(IDirect3DVertexBuffer9 *vb, UINT off, UINT size, void **pp, DWORD fl)
{
    HRESULT hr = o_VBLock(vb, off, size, pp, fl);
    if (g_trace_on && SUCCEEDED(hr) && pp && *pp && !(fl & D3DLOCK_READONLY) && g_trace_locks < 4000 && g_nsh < 32) {
        D3DVERTEXBUFFER_DESC d; UINT sz = size; BYTE *sh;
        if (!sz) { if (FAILED(IDirect3DVertexBuffer9_GetDesc(vb, &d))) return hr; sz = d.Size - off; }
        if (sz > 4 * 1024 * 1024) return hr;
        sh = (BYTE *)VirtualAlloc(NULL, sz + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!sh) return hr;
        if (!(fl & D3DLOCK_DISCARD)) memcpy(sh, *pp, sz);
        g_sh[g_nsh].vb = vb; g_sh[g_nsh].real = (BYTE *)*pp; g_sh[g_nsh].shadow = sh; g_sh[g_nsh].size = sz; g_nsh++;
        { DWORD old; VirtualProtect(sh, 4096, PAGE_READWRITE | PAGE_GUARD, &old); }
        *pp = sh; g_trace_locks++;
    }
    return hr;
}
static HRESULT WINAPI h_VBUnlock(IDirect3DVertexBuffer9 *vb)
{
    int i;
    for (i = 0; i < g_nsh; i++) if (g_sh[i].vb == vb) {
        memcpy(g_sh[i].real, g_sh[i].shadow, g_sh[i].size); VirtualFree(g_sh[i].shadow, 0, MEM_RELEASE);
        g_sh[i] = g_sh[--g_nsh]; break;
    }
    return o_VBUnlock(vb);
}
static void trace_install(IDirect3DDevice9 *dev)
{
    IDirect3DVertexBuffer9 *vb = NULL; void *o;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, trace_veh);
    if (SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(dev, 64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL)) && vb) {
        if ((o = patch_vt(vb, 11, h_VBLock))) o_VBLock = (PF_VBLOCK)o;
        if ((o = patch_vt(vb, 12, h_VBUnlock))) o_VBUnlock = (PF_VBUNLOCK)o;
        IDirect3DVertexBuffer9_Release(vb);
    }
    logmsg("writer trace armed: Lock hooked=%d Unlock hooked=%d", o_VBLock != NULL, o_VBUnlock != NULL);
}
static void trace_report(void)
{
    int i, k; logmsg("writer trace: %d locks guarded, %d distinct writer instructions", g_trace_locks, g_nwr);
    for (i = 0; i < g_nwr; i++) {
        logmsg("  writer eip=%08lx hits=%u callers:", g_wr[i].eip, g_wr[i].n);
        for (k = 0; k < 6 && g_wr[i].caller[k]; k++) logmsg("      from %08lx", g_wr[i].caller[k]);
    }
}


/* ---------------- file open logging (reverse engineering aid): ini LogFiles=1 ---------------- */
static HANDLE (WINAPI *r_CFA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *r_CFW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static FILE *g_filelog; static unsigned g_nfile;
static void flog(const char *kind, const char *path, DWORD disp, HANDLE h)
{
    if (!g_filelog || g_nfile > 6000) return;
    /* skip the proxy's own files and wine system noise */
    if (strstr(path, "TheMoviesGfx") || strstr(path, "\\windows\\") || strstr(path, "/proc/")) return;
    fprintf(g_filelog, "%s %s%s -> %s\n", kind, path, "", (h == INVALID_HANDLE_VALUE) ? "NOT FOUND" : "opened"); fflush(g_filelog); g_nfile++;
}
static HANDLE WINAPI hk_CreateFileA(LPCSTR p, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) { HANDLE h = r_CFA(p, a, sh, sa, d, f, t); if (p) flog("A", p, d, h); return h; }
static HANDLE WINAPI hk_CreateFileW(LPCWSTR p, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    HANDLE h = r_CFW(p, a, sh, sa, d, f, t); char u[600];
    if (p && g_filelog) { WideCharToMultiByte(CP_UTF8, 0, p, -1, u, sizeof u, NULL, NULL); flog("W", u, d, h); }
    return h;
}
static void filelog_install(void)
{
    char t[MAX_PATH]; HMODULE exe = GetModuleHandleA(NULL);
    if (g_filelog) return;
    strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.files.log"); g_filelog = fopen(t, "w");
    patch_iat_mod(exe, "kernel32.dll", "CreateFileA", hk_CreateFileA, (void **)&r_CFA);
    patch_iat_mod(exe, "kernel32.dll", "CreateFileW", hk_CreateFileW, (void **)&r_CFW);
    logmsg("file logging on (CreateFileA=%p W=%p)", (void *)r_CFA, (void *)r_CFW);
}

/* ---------------- hooks ---------------- */
static HRESULT WINAPI hk_Present(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    { static LARGE_INTEGER lt, fq; static double mx, sum; static int n33, n50, n100, nf; LARGE_INTEGER nw; if (!fq.QuadPart) QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&nw);
      if (lt.QuadPart) { double ms = (double)(nw.QuadPart - lt.QuadPart) * 1000.0 / (double)fq.QuadPart; if (ms > mx) mx = ms; sum += ms; nf++; if (ms > 33.4) n33++; if (ms > 50.0) n50++; if (ms > 100.0) n100++;
        if (nf >= 300) { logmsg("mk_draw: %lu calls, %lu vertices, %.1f ms total over the same frames (glyph quads %lu, cache hits %lu)", g_mkp_calls, g_mkp_verts, g_mkp_ms, g_mkq, g_mkq_hit); g_mkq = g_mkq_hit = 0; { LARGE_INTEGER fq2; double k; int i2; QueryPerformanceFrequency(&fq2); k = 1000.0 / (double)fq2.QuadPart; logmsg("mk_draw breakdown over %lu processed draws (ms): tex %.1f  getstate %.1f  vblock %.1f  indexcopy %.1f  quads %.1f  ring+draw %.1f  restore %.1f  release %.1f", g_mk_proc, g_mkt[0] * k, g_mkt[1] * k, g_mkt[2] * k, 0.0, g_mkt[3] * k, g_mkt[4] * k, g_mkt[5] * k, g_mkt[6] * k); for (i2 = 0; i2 < 8; i2++) g_mkt[i2] = 0; g_mk_proc = 0; } g_mkp_calls = g_mkp_verts = 0; g_mkp_ms = 0; logmsg("FRAMETIME over %d frames: avg %.2f ms (%.0f fps) max %.1f ms; >33ms: %d  >50ms: %d  >100ms: %d", nf, sum / nf, 1000.0 * nf / sum, mx, n33, n50, n100); mx = sum = 0; n33 = n50 = n100 = nf = 0; } }
      lt = nw; }
    G.frame++;
    if (dev == G.dev && G.frame % 600 == 0) {
        static DWORD t0; DWORD now = GetTickCount();
        if (t0) logmsg("mouse calls: GetCursorPos %u ScreenToClient %u Peek %u", g_ncp, g_ns2c, g_npeek); logmsg("%.1f fps over last 600 frames (ui effect %s); sprites zoomed %lu, exempt (large) %lu, unscaled %lu", 600000.0 / (now - t0 ? now - t0 : 1), ui_enabled() ? "ON" : "off", V.zoomed, V.exempt, V.skipped);
        t0 = now;
    }
    if (dev == G.dev) {
        static int prev_f12;
        int f12 = key(VK_CONTROL) && key(VK_SHIFT) && (key(VK_F12) || key(VK_F11));
        if (f12 && !prev_f12 && !g_pf) probe_begin();
        prev_f12 = f12;
        if (G.frame % 20 == 0) { /* scan trigger: file TheMoviesGfx.scan.now containing "sx sy" (floats) */
            char t[MAX_PATH]; FILE *sf; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.scan.now");
            if ((sf = fopen(t, "r"))) { float sx = 0, sy = 0; if (fscanf(sf, "%f %f", &sx, &sy) == 2) { fclose(sf); DeleteFileA(t); if (sy < 0) mem_scan_one(sx, "single"); else mem_scan(sx, sy); } else { fclose(sf); DeleteFileA(t); } }
        }
        if (G.frame % 20 == 10) { /* peek trigger: file TheMoviesGfx.peek.now containing "hexaddr count" */
            char t[MAX_PATH]; FILE *pf; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.peek.now");
            if ((pf = fopen(t, "r"))) { unsigned long a = 0; int c = 0; if (fscanf(pf, "%lx %d", &a, &c) == 2) { fclose(pf); DeleteFileA(t); if (c > 0 && c <= 256) mem_peek(a, c); } else { fclose(pf); DeleteFileA(t); } }
        }
        if (G.frame % 20 == 5) { /* poke trigger: file TheMoviesGfx.poke.now with lines "hexaddr float" */
            char t[MAX_PATH]; FILE *pf; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.poke.now");
            if ((pf = fopen(t, "r"))) { unsigned long a; float v; while (fscanf(pf, "%lx %f", &a, &v) == 2) mem_poke(a, v); fclose(pf); DeleteFileA(t); }
        }
        { /* auto mode: file TheMoviesGfx.curscan.auto switches it on (the file is deleted); scans whenever the cursor rested >=30 frames at a spot >=100 px from the last scan */
          static int armed, still; static float lx = -9999, ly = -9999, sx0 = -9999, sy0 = -9999; static int nauto;
          if (G.frame % 20 == 7) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.curscan.auto"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); armed = 1; nauto = 0; g_csinit = 0; logmsg("curscan: auto mode armed"); } }
          if (armed && g_cur_ok) {
              if (g_cur_x == lx && g_cur_y == ly) still++; else { still = 0; lx = g_cur_x; ly = g_cur_y; }
              if (still == 30 && lx > 40 && ly > 40 && (fabsf(lx - sx0) >= 100 || fabsf(ly - sy0) >= 100)) { sx0 = lx; sy0 = ly; cursor_scan(); if (++nauto >= 5) { armed = 0; logmsg("curscan: auto mode done"); } }
          }
        }
        if (G.frame % 20 == 3) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.curscan.now"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); cursor_scan(); } }
        if (G.frame % 20 == 11) { char t[MAX_PATH]; FILE *wf; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.watch.now"); if ((wf = fopen(t, "r"))) { unsigned long a0 = 0, a1 = 0; int n; int c0 = fgetc(wf); if (c0 == 'r') { g_watch_rw = 1; } else { g_watch_rw = 0; ungetc(c0, wf); } n = fscanf(wf, "%lx %lx", &a0, &a1); fclose(wf); DeleteFileA(t); if (n >= 1) watch_start(a0, n >= 2 ? a1 : 0); } }
        if (G.frame % 20 == 13) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.widgets.now"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); widget_dump(); } }
        if (G.frame % 20 == 9) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.thumb2.now"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); g_thumb_arm2 = 1; } }
        if (G.frame % 20 == 7) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.atlas.now"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); g_atlas_arm = 1; } }
        if (G.frame % 20 == 11) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.inkdump.now"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); g_ink_reset = 1; } }
        if (G.frame % 20 == 3) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.framedump.now"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); g_fd_on = G.frame + 2; logmsg("framedump armed for frame %lu", g_fd_on); } }
        if (G.frame % 20 == 9) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.edge.on"); g_edge_on = (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES); }
        if (G.frame == 8) { movie_res_patch(); if (g_exp_on) export_width_patch(); }
        if (G.frame % 20 == 17) { char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.widgets.all"); if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); widget_dump_all(); } }
        if (G.frame % 20 == 5) { /* vtable scan trigger: file TheMoviesGfx.vtscan.now = list of hex vtables -> TheMoviesGfx.vtscan.txt (every heap object starting with one of them, with its edge records) */
            char t[MAX_PATH], o[MAX_PATH]; FILE *tf; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.vtscan.now"); strcpy(o, g_logp); strcpy(strrchr(o, '\\') + 1, "TheMoviesGfx.vtscan.txt");
            if ((tf = fopen(t, "r")) != NULL) {
                unsigned long vts[16]; int nv = 0; char tok[32]; FILE *of; SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi; BYTE *a2; BYTE *buf = (BYTE *)malloc(SCAN_CHUNK + 0x120);
                while (nv < 16 && fscanf(tf, "%31s", tok) == 1) vts[nv++] = strtoul(tok, NULL, 16);
                fclose(tf); DeleteFileA(t); of = fopen(o, "w"); GetSystemInfo(&si);
                if (of && buf) for (a2 = (BYTE *)si.lpMinimumApplicationAddress; a2 < (BYTE *)si.lpMaximumApplicationAddress; a2 += mbi.RegionSize) {
                    if (!VirtualQuery(a2, &mbi, sizeof mbi)) break;
                    if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) && mbi.Type == MEM_PRIVATE && mbi.RegionSize < 0x10000000) {
                        SIZE_T off, rsz = mbi.RegionSize;
                        for (off = 0; off + 0x110 < rsz; off += SCAN_CHUNK) {
                            SIZE_T n = rsz - off < SCAN_CHUNK + 0x110 ? rsz - off : SCAN_CHUNK + 0x110, got = 0, lim, ci; int k;
                            if (!ReadProcessMemory(GetCurrentProcess(), (BYTE *)mbi.BaseAddress + off, buf, n, &got) || got < 0x120) continue;
                            lim = got - 0x110; if (lim > SCAN_CHUNK) lim = SCAN_CHUNK;
                            for (ci = 0; ci < lim; ci += 4) { DWORD vt = *(DWORD *)(buf + ci); for (k = 0; k < nv; k++) if (vt == vts[k]) {
                                BYTE *cp = buf + ci; fprintf(of, "%p vt=%08lx in(t%g l%g b%g r%g) abs(t%g l%g b%g r%g) par %08lx\n", (void *)((BYTE *)mbi.BaseAddress + off + ci), (unsigned long)vt,
                                    *(float *)(cp + 0x98), *(float *)(cp + 0xbc), *(float *)(cp + 0xe0), *(float *)(cp + 0x104), *(float *)(cp + 0x9c), *(float *)(cp + 0xc0), *(float *)(cp + 0xe4), *(float *)(cp + 0x108), *(unsigned long *)(cp + 0xb8)); } }
                        }
                    }
                }
                if (of) fclose(of); free(buf);
            }
        }
        if (G.frame % 20 == 9) { /* memory dump trigger: file TheMoviesGfx.mem.now = "hexaddr [ndwords]" (several lines allowed) -> TheMoviesGfx.mem.txt */
            char t[MAX_PATH], o[MAX_PATH]; FILE *tf; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.mem.now"); strcpy(o, g_logp); strcpy(strrchr(o, '\\') + 1, "TheMoviesGfx.mem.txt");
            if ((tf = fopen(t, "r")) != NULL) {
                FILE *of = fopen(o, "w"); char ln[128]; unsigned long ad; int n;
                while (of && fgets(ln, sizeof ln, tf)) {
                    n = 96; if (sscanf(ln, "%lx %d", &ad, &n) < 1) continue; if (n > 1024) n = 1024;
                    fprintf(of, "== %08lx x%d\n", ad, n);
                    { DWORD *buf = (DWORD *)malloc(n * 4 + 8); SIZE_T got = 0; int i;
                      if (buf && ReadProcessMemory(GetCurrentProcess(), (void *)ad, buf, n * 4, &got)) for (i = 0; i < (int)(got / 4); i++) { float f; memcpy(&f, &buf[i], 4); fprintf(of, "%03x: %08lx  %g\n", i * 4, (unsigned long)buf[i], (double)f); }
                      else fprintf(of, "unreadable\n");
                      free(buf); }
                }
                if (of) fclose(of); fclose(tf); DeleteFileA(t);
            }
        }
        if (G.frame % 20 == 15) { /* trace trigger: file TheMoviesGfx.trace.now arms the writer trace for ~2 s */
            char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.trace.now");
            if (!g_trace_on && GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); g_nwr = 0; g_trace_locks = 0; trace_install(dev); g_trace_on = (int)(G.frame ? G.frame : 1); }
        }
        if (g_trace_on > 1 && G.frame > (unsigned long)g_trace_on + 240) { g_trace_on = 0; trace_report(); }
        if (!g_pf && G.frame % 20 == 0) { /* file trigger: create TheMoviesGfx.probe.now next to the dll */
            char t[MAX_PATH]; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.probe.now");
            if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(t); probe_begin(); }
        }
        if (g_pf && --g_probe_left < 0) probe_end();
        else if (g_pf) plog("---- Present #%lu ----", G.frame);
    }
    if (dev == G.dev && G.failed < 30) {
        FILETIME ft;
        if (g_page_root && G.frame - g_page_frame > 90) g_page_root = NULL; /* page left: the next visit builds a new panel */
        if (G.frame - g_page_frame <= 2 && !(g_page_root && !IsBadReadPtr(g_page_root, 0x110) && *(DWORD *)g_page_root == 0x00d3358c) && !g_wscan_busy && GetTickCount() - g_wscan_tick > 800) { g_wscan_tick = GetTickCount(); InterlockedExchange(&g_wscan_busy, 1); CreateThread(NULL, 0, widget_scan_thread, NULL, 0, NULL); } /* the page just appeared: find its panel at once */
        if (cfg_panelh > 100.0f && g_page_root && !IsBadReadPtr(g_page_root, 0x110) && *(DWORD *)g_page_root == 0x00d3358c) { *(volatile float *)(g_page_root + 0xe0) = cfg_panelh; *(volatile float *)(g_page_root + 0x98) = 0.5f * (768.0f - cfg_panelh) + cfg_paneldy; }
        hotkeys();
        { /* automation aid: while TheMoviesGfx.cursor.on exists, publish the game's pointer position (game px) in TheMoviesGfx.cursor.txt */
            static int on; char t[MAX_PATH];
            if (G.frame % 30 == 0) { strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.cursor.on"); on = GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES; }
            if (on && G.frame % 4 == 0 && !IsBadReadPtr((void *)0x0104CCE0, 8)) {
                FILE *cf; strcpy(t, g_logp); strcpy(strrchr(t, '\\') + 1, "TheMoviesGfx.cursor.txt");
                if ((cf = fopen(t, "w")) != NULL) { fprintf(cf, "%.1f %.1f %lu\n", g_pm_on ? g_pm_real[0] : *(volatile float *)0x0104CCE0, g_pm_on ? g_pm_real[1] : *(volatile float *)0x0104CCE4, G.frame); fclose(cf); }
            }
        }
        if (g_dirty_tick && GetTickCount() - g_dirty_tick > 2000) { g_dirty_tick = 0; save_ini(); }
        else if (!g_dirty_tick && G.frame % 30 == 0 && file_time(&ft) && CompareFileTime(&ft, &g_ini_time)) { load_ini(); logmsg("ini reloaded"); }
        if (g_cfg.enabled && (g_cfg.bright != 0.0f || g_cfg.contrast != 1.0f || g_cfg.gamma != 1.0f || g_cfg.sat != 1.0f || g_cfg.sharpen > 0.0f || g_cfg.fxaa))
            post_process(dev);
        ov_draw(dev);
    }
    return orig_Present(dev, a, b, w, r);
}
static void apply_msaa(D3DPRESENT_PARAMETERS *pp);
static HRESULT WINAPI hk_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    logmsg("Reset %ux%u", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    if (dev == G.dev) res_release();
    mk_ring_release();
    if (g_ov_sb) { IDirect3DStateBlock9_Release(g_ov_sb); g_ov_sb = NULL; }
    if (pp) {
        D3DPRESENT_PARAMETERS local = *pp; HRESULT hr; UINT gw = pp->BackBufferWidth, gh = pp->BackBufferHeight;
        virtual_adjust_pp(&local); apply_msaa(&local);
        hr = orig_Reset(dev, &local);
        *pp = local; if (V.on) { pp->BackBufferWidth = gw; pp->BackBufferHeight = gh; } /* the game keeps seeing the size it asked for */
        return hr;
    }
    return orig_Reset(dev, pp);
}
/* MSAA option (ini MSAA = 0/2/4/8, needs a game restart): multisampled back buffer + auto depth buffer, only if the driver offers it for both formats; the back buffer is resolved by the post-process StretchRect / present. Falls back silently to no MSAA. */
static IDirect3D9 *g_d3d9; static UINT g_dev_ad; static D3DDEVTYPE g_dev_type;
static void apply_msaa(D3DPRESENT_PARAMETERS *pp)
{
    int ms = (int)getf("MSAA", 0.0f); DWORD q = 0; D3DMULTISAMPLE_TYPE mt;
    if (!pp || !g_d3d9 || ms < 2 || ms > 8 || pp->Windowed) { if (pp && ms >= 2 && pp->Windowed) logmsg("MSAA: windowed mode, skipped"); return; }
    mt = (D3DMULTISAMPLE_TYPE)ms;
    if (FAILED(IDirect3D9_CheckDeviceMultiSampleType(g_d3d9, g_dev_ad, g_dev_type, pp->BackBufferFormat, pp->Windowed, mt, &q))) { logmsg("MSAA %dx: not supported for the back buffer format", ms); return; }
    if (pp->EnableAutoDepthStencil && FAILED(IDirect3D9_CheckDeviceMultiSampleType(g_d3d9, g_dev_ad, g_dev_type, pp->AutoDepthStencilFormat, pp->Windowed, mt, &q))) { logmsg("MSAA %dx: not supported for the depth format", ms); return; }
    pp->MultiSampleType = mt; pp->MultiSampleQuality = 0; pp->SwapEffect = D3DSWAPEFFECT_DISCARD; pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    logmsg("MSAA %dx enabled on the back buffer", ms);
}
static HRESULT WINAPI hk_CreateDevice(IDirect3D9 *d3d, UINT ad, D3DDEVTYPE t, HWND hwnd, DWORD fl, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr; D3DPRESENT_PARAMETERS local; UINT gw = pp ? pp->BackBufferWidth : 0, gh = pp ? pp->BackBufferHeight : 0;
    g_d3d9 = d3d; g_dev_ad = ad; g_dev_type = t;
    if (pp) { local = *pp; virtual_adjust_pp(&local); apply_msaa(&local); } else V.on = 0;
    if (V.on) { G.hwnd = hwnd; subclass_window(hwnd); } /* before CreateDevice: the fullscreen switch sends WM_SIZE from inside it */
    hr = orig_CreateDevice(d3d, ad, t, hwnd, fl, pp ? &local : pp, out);
    if (pp) { *pp = local; if (V.on) { pp->BackBufferWidth = gw; pp->BackBufferHeight = gh; } } /* the game keeps seeing the size it asked for */
    logmsg("CreateDevice -> %lx (device back buffer %ux%u, game sees %ux%u, windowed=%d)", hr, pp ? local.BackBufferWidth : 0, pp ? local.BackBufferHeight : 0, gw, gh, pp ? pp->Windowed : 0);
    if (SUCCEEDED(hr) && out && *out) {
        void *o;
        res_release();
        if (G.ps) { IDirect3DPixelShader9_Release(G.ps); G.ps = NULL; }
        if (G.ps_fx) { IDirect3DPixelShader9_Release(G.ps_fx); G.ps_fx = NULL; }
        G.dev = *out; G.hwnd = hwnd; G.failed = 0; G.logged = 0;
        if ((o = patch_vt(*out, 17, hk_Present))) orig_Present = (PFN_Present)o;
        if ((o = patch_vt(*out, 16, hk_Reset))) orig_Reset = (PFN_Reset)o;
        probe_hook_device(*out);
        if ((int)getf("LogFiles", 0)) filelog_install();
        virtual_install_input_hooks(); if (V.on) install_size_hooks(*out);
        install_mode_patch();
        logmsg("device hooked (Present=%p)", orig_Present);
    }
    return hr;
}

/* ---------------- exports ---------------- */
IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d; void *o;
    if (!load_real()) return NULL;
    install_mode_patch(); /* before the game builds its list of display modes */
    d3d = real_Create9(sdk);
    if (d3d) { UINT n = IDirect3D9_GetAdapterModeCount(d3d, 0, D3DFMT_X8R8G8B8), i; char buf[1024] = ""; UINT seen[64][2], ns = 0;
        for (i = 0; i < n; i++) { D3DDISPLAYMODE m; UINT j, dup = 0; if (FAILED(IDirect3D9_EnumAdapterModes(d3d, 0, D3DFMT_X8R8G8B8, i, &m))) continue; for (j = 0; j < ns; j++) if (seen[j][0] == m.Width && seen[j][1] == m.Height) dup = 1; if (!dup && ns < 64) { char t[24]; seen[ns][0] = m.Width; seen[ns][1] = m.Height; ns++; sprintf(t, "%ux%u ", m.Width, m.Height); if (strlen(buf) < 980) strcat(buf, t); } }
        logmsg("display modes offered by the driver (%u total, unique sizes): %s", n, buf); }
    if (d3d && (o = patch_vt(d3d, 16, hk_CreateDevice))) { orig_CreateDevice = (PFN_CreateDevice)o; logmsg("IDirect3D9 hooked"); }
    if (d3d && (o = patch_vt(d3d, 8, h_AGDM))) o_AGDM = (PF_AGDM)o;
    return d3d;
}
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **o)
{
    if (!load_real() || !real_Create9Ex) return E_NOTIMPL;
    return real_Create9Ex(sdk, o);
}
int WINAPI D3DPERF_BeginEvent(DWORD c, LPCWSTR n) { return 0; }
int WINAPI D3DPERF_EndEvent(void) { return 0; }
void WINAPI D3DPERF_SetMarker(DWORD c, LPCWSTR n) {}
void WINAPI D3DPERF_SetRegion(DWORD c, LPCWSTR n) {}
BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD o) {}
DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
void WINAPI DebugSetMute(void) {}
void WINAPI DebugSetLevel(DWORD l) {}

/* The game writes HKCU\\Software\\Lionhead Studios Ltd\\TheMovies "Game running:" = Yes at start and clears it on a clean exit; after a crash or kill the next start asks to reset the graphics
 * to low. That check runs before any of our DLLs load, so instead keep the flag empty while the game runs: whatever way it ends, the next start has no prompt. */
static DWORD WINAPI runflag_thread(LPVOID p)
{
    for (;;) {
        HKEY k; char cur[16] = ""; DWORD n = sizeof cur - 1, ty = 0;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Lionhead Studios Ltd\\TheMovies", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "Game running:", NULL, &ty, (BYTE *)cur, &n) == ERROR_SUCCESS && cur[0]) { RegSetValueExA(k, "Game running:", 0, REG_SZ, (const BYTE *)"", 1); logmsg("cleared 'Game running:' flag (was '%s')", cur); }
            RegCloseKey(k);
        }
        Sleep(2000);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char *s;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, g_ini, MAX_PATH);
        s = strrchr(g_ini, '\\'); if (s) *s = 0;
        sprintf(g_logp, "%s\\TheMoviesGfx.log", g_ini);
        lstrcatA(g_ini, "\\TheMoviesGfx.ini");
        write_default_ini(); load_ini();
        remove(g_logp);
        logmsg("proxy d3d9 loaded: enabled=%d bright=%.3f contrast=%.3f gamma=%.3f sat=%.3f", g_cfg.enabled, g_cfg.bright, g_cfg.contrast, g_cfg.gamma, g_cfg.sat);
        CloseHandle(CreateThread(NULL, 0, runflag_thread, NULL, 0, NULL));
    }
    return TRUE;
}
