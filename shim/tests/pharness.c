/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reader -> Writer harness driven by a real game profile file (UTF-8 .xml, e.g. extracted WMVProfile_Best.prx).
 * usage: pharness.exe <input.wmv> <output.wmv> <profile.xml> [timeout_ms]
 * Run from a directory holding asfreader.dll and wmvcore.dll. */
#define COBJMACROS
#define STRSAFE_NO_DEPRECATE
#include <windows.h>
#include <dshow.h>
#include <wmsdk.h>
#include <stdio.h>
#include <stdlib.h>
static const GUID CLSID_Reader={0x187463a0,0x5bb7,0x11d3,{0xac,0xbe,0x00,0x80,0xc7,0x5e,0x24,0x6e}};
static const GUID CLSID_Writer={0x7c23220e,0x55bb,0x11d3,{0x8b,0x16,0x00,0xc0,0x4f,0xb6,0xbd,0x3d}};
static const GUID IID_ICfg={0x45086030,0xf7e4,0x486a,{0xb5,0x04,0x82,0x6b,0xb5,0x79,0x2a,0x3b}};
typedef HRESULT (WINAPI *GCO)(REFCLSID,REFIID,void**);
typedef HRESULT (WINAPI *PMC)(IWMProfileManager**);
static IPin* nth(IBaseFilter*f,int i,PIN_DIRECTION want){IEnumPins*e;IPin*p;int k=0;IBaseFilter_EnumPins(f,&e);while(IEnumPins_Next(e,1,&p,0)==S_OK){PIN_DIRECTION d;IPin_QueryDirection(p,&d);if(d==want){if(k==i){IEnumPins_Release(e);return p;}k++;}IPin_Release(p);}IEnumPins_Release(e);return 0;}
static char *slurp(const char *path, long *n){FILE*f=fopen(path,"rb");if(!f)return 0;fseek(f,0,SEEK_END);*n=ftell(f);fseek(f,0,SEEK_SET);char*b=malloc(*n+1);fread(b,1,*n,f);b[*n]=0;fclose(f);return b;}
int main(int argc,char**argv){
  if(argc<4){printf("usage: %s in.wmv out.wmv profile.xml [timeout_ms]\n",argv[0]);return 2;}
  long tmo=argc>4?atol(argv[4]):120000;
  CoInitialize(0);
  HMODULE m=LoadLibraryA("asfreader.dll"); GCO g=(GCO)GetProcAddress(m,"DllGetClassObject");
  HMODULE w=LoadLibraryA("wmvcore.dll"); PMC pmc=(PMC)GetProcAddress(w,"WMCreateProfileManager");
  IClassFactory*cf; IBaseFilter*rd,*wr;
  g(&CLSID_Reader,&IID_IClassFactory,(void**)&cf); IClassFactory_CreateInstance(cf,0,&IID_IBaseFilter,(void**)&rd);
  g(&CLSID_Writer,&IID_IClassFactory,(void**)&cf); HRESULT hr=IClassFactory_CreateInstance(cf,0,&IID_IBaseFilter,(void**)&wr); printf("writer %lx\n",hr);
  IGraphBuilder*gb; CoCreateInstance(&CLSID_FilterGraph,0,CLSCTX_INPROC_SERVER,&IID_IGraphBuilder,(void**)&gb);
  IGraphBuilder_AddFilter(gb,rd,L"rd"); IGraphBuilder_AddFilter(gb,wr,L"wr");
  IFileSinkFilter*sink; IBaseFilter_QueryInterface(wr,&IID_IFileSinkFilter,(void**)&sink); WCHAR of[260]; MultiByteToWideChar(CP_ACP,0,argv[2],-1,of,260); hr=IFileSinkFilter_SetFileName(sink,of,0); printf("setfile %lx\n",hr);
  long pn; char *prx=slurp(argv[3],&pn); if(!prx){printf("cannot read profile\n");return 2;}
  WCHAR *wprx=malloc((pn+1)*2); MultiByteToWideChar(CP_UTF8,0,prx,-1,wprx,pn+1);
  IWMProfileManager*pm; pmc(&pm); IWMProfile*prof; hr=IWMProfileManager_LoadProfileByData(pm,wprx,&prof); printf("loadprofile %lx\n",hr);
  DWORD ns=0; IWMProfile_GetStreamCount(prof,&ns); printf("profile streams %lu\n",ns);
  struct ICfg{const struct V{HRESULT(WINAPI*q)(void*,REFIID,void**);ULONG(WINAPI*a)(void*);ULONG(WINAPI*r)(void*);HRESULT(WINAPI*c1)(void*,DWORD);HRESULT(WINAPI*c2)(void*,DWORD*);HRESULT(WINAPI*c3)(void*,REFGUID);HRESULT(WINAPI*c4)(void*,GUID*);HRESULT(WINAPI*c5)(void*,IWMProfile*);} *v;} *cfg;
  hr=IBaseFilter_QueryInterface(wr,&IID_ICfg,(void**)&cfg); printf("qi cfg %lx\n",hr); hr=cfg->v->c5(cfg,prof); printf("configure %lx\n",hr);
  IFileSourceFilter*fs; IBaseFilter_QueryInterface(rd,&IID_IFileSourceFilter,(void**)&fs); WCHAR wf[512]; MultiByteToWideChar(CP_ACP,0,argv[1],-1,wf,512); hr=IFileSourceFilter_Load(fs,wf,0); printf("load %lx\n",hr);
  for(int i=0;i<2;i++){ IPin*o=nth(rd,i,PINDIR_OUTPUT); IPin*in=nth(wr,i,PINDIR_INPUT); IPin*in2=nth(wr,1-i,PINDIR_INPUT);
    hr=IGraphBuilder_Connect(gb,o,in); if(FAILED(hr)&&in2) hr=IGraphBuilder_Connect(gb,o,in2); printf("connect %d %lx\n",i,hr);}
  IMediaControl*mc; IMediaEvent*me; IGraphBuilder_QueryInterface(gb,&IID_IMediaControl,(void**)&mc); IGraphBuilder_QueryInterface(gb,&IID_IMediaEvent,(void**)&me);
  hr=IMediaControl_Run(mc); printf("run %lx\n",hr);
  long ec; hr=IMediaEvent_WaitForCompletion(me,tmo,&ec); printf("wait %lx ec=%ld\n",hr,ec);
  hr=IMediaControl_Stop(mc); printf("stop %lx\n",hr);
  printf("done\n"); return 0;}
