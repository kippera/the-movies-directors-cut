/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stage-2 audio chain as the game builds it: AsyncReader(WAV) -> Wav Parser -> "Audio Enc" (via devenum) -> ASF writer.
 * usage: chaintest.exe in.wav out.wmv profile.xml   (needs asfreader.dll + wmvcore.dll + chaintest.exe.manifest registering
 * SystemDeviceEnum, FileWriter and ASF writer CLSIDs to asfreader.dll) */
#define COBJMACROS
#include <windows.h>
#include <dshow.h>
#include <wmsdk.h>
#include <stdio.h>
#include <stdlib.h>
static const GUID CLSID_Async={0xe436ebb5,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID CLSID_WavParser={0xd51bd5a1,0x7548,0x11cf,{0xa5,0x20,0x00,0x80,0xc7,0x7e,0xf5,0x8a}};
static const GUID CLSID_Writer={0x7c23220e,0x55bb,0x11d3,{0x8b,0x16,0x00,0xc0,0x4f,0xb6,0xbd,0x3d}};
static const GUID CLSID_SDE={0x62BE5D10,0x60EB,0x11d0,{0xBD,0x3B,0x00,0xA0,0xC9,0x11,0xCE,0x86}};
static const GUID CAT_A={0x33D9A761,0x90C8,0x11d0,{0xBD,0x43,0x00,0xA0,0xC9,0x11,0xCE,0x86}};
static const GUID IID_ICfg={0x45086030,0xf7e4,0x486a,{0xb5,0x04,0x82,0x6b,0xb5,0x79,0x2a,0x3b}};
typedef HRESULT (WINAPI *PMC)(IWMProfileManager**);
static IPin* nth(IBaseFilter*f,int i,PIN_DIRECTION want){IEnumPins*e;IPin*p;int k=0;IBaseFilter_EnumPins(f,&e);while(IEnumPins_Next(e,1,&p,0)==S_OK){PIN_DIRECTION d;IPin_QueryDirection(p,&d);if(d==want){if(k==i){IEnumPins_Release(e);return p;}k++;}IPin_Release(p);}IEnumPins_Release(e);return 0;}
int main(int argc,char**argv){
  CoInitialize(0); HRESULT hr; IGraphBuilder*gb; CoCreateInstance(&CLSID_FilterGraph,0,CLSCTX_INPROC_SERVER,&IID_IGraphBuilder,(void**)&gb);
  IBaseFilter *rd,*wp,*ae=0,*wr; WCHAR w[512];
  hr=CoCreateInstance(&CLSID_Async,0,1,&IID_IBaseFilter,(void**)&rd); printf("async %lx\n",hr); IGraphBuilder_AddFilter(gb,rd,L"File Reader for WAV");
  IFileSourceFilter*fs; IBaseFilter_QueryInterface(rd,&IID_IFileSourceFilter,(void**)&fs); MultiByteToWideChar(CP_ACP,0,argv[1],-1,w,512); hr=IFileSourceFilter_Load(fs,w,0); printf("load wav %lx\n",hr);
  hr=CoCreateInstance(&CLSID_WavParser,0,1,&IID_IBaseFilter,(void**)&wp); printf("wavparser %lx\n",hr); IGraphBuilder_AddFilter(gb,wp,L"Wav Parser");
  ICreateDevEnum*de; IEnumMoniker*em; CoCreateInstance(&CLSID_SDE,0,1,&IID_ICreateDevEnum,(void**)&de); hr=ICreateDevEnum_CreateClassEnumerator(de,&CAT_A,&em,0); printf("cat %lx\n",hr);
  IMoniker*m; while(hr==S_OK&&IEnumMoniker_Next(em,1,&m,0)==S_OK){IPropertyBag*pb;VARIANT v;VariantInit(&v); if(SUCCEEDED(IMoniker_BindToStorage(m,0,0,&IID_IPropertyBag,(void**)&pb))&&SUCCEEDED(IPropertyBag_Read(pb,L"FriendlyName",&v,0))&&!wcscmp(V_BSTR(&v),L"Microsoft ADPCM")){hr=IMoniker_BindToObject(m,0,0,&IID_IBaseFilter,(void**)&ae);break;}}
  printf("audio enc %p\n",ae); if(!ae) return 1; IGraphBuilder_AddFilter(gb,ae,L"Audio Enc");
  hr=CoCreateInstance(&CLSID_Writer,0,1,&IID_IBaseFilter,(void**)&wr); printf("writer %lx\n",hr); IGraphBuilder_AddFilter(gb,wr,L"ASF Enc/Writer");
  IFileSinkFilter*sink; IBaseFilter_QueryInterface(wr,&IID_IFileSinkFilter,(void**)&sink); MultiByteToWideChar(CP_ACP,0,argv[2],-1,w,512); IFileSinkFilter_SetFileName(sink,w,0);
  FILE*f=fopen(argv[3],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); char*x=malloc(n+1); fread(x,1,n,f); x[n]=0; WCHAR*wx=malloc((n+1)*2); MultiByteToWideChar(CP_UTF8,0,x,-1,wx,n+1);
  HMODULE wm=LoadLibraryA("wmvcore.dll"); PMC pmc=(PMC)GetProcAddress(wm,"WMCreateProfileManager"); IWMProfileManager*pm; pmc(&pm); IWMProfile*prof; hr=IWMProfileManager_LoadProfileByData(pm,wx,&prof); printf("profile %lx\n",hr);
  struct ICfg{const struct V{void*q,*a,*r,*c1,*c2,*c3,*c4; HRESULT(WINAPI*c5)(void*,IWMProfile*);} *v;} *cfg; IBaseFilter_QueryInterface(wr,&IID_ICfg,(void**)&cfg); hr=cfg->v->c5(cfg,prof); printf("configure %lx\n",hr);
  IPin*a=nth(wp,0,PINDIR_OUTPUT),*b=nth(ae,0,PINDIR_INPUT); printf("pins wp.out=%p ae.in=%p\n",a,b);
  hr=IGraphBuilder_Connect(gb,nth(rd,0,PINDIR_OUTPUT),nth(wp,0,PINDIR_INPUT)); printf("reader->parser %lx\n",hr);
  a=nth(wp,0,PINDIR_OUTPUT); hr=IGraphBuilder_Connect(gb,a,nth(ae,0,PINDIR_INPUT)); printf("parser->enc %lx\n",hr);
  hr=IGraphBuilder_Connect(gb,nth(ae,0,PINDIR_OUTPUT),nth(wr,0,PINDIR_INPUT)); printf("enc->writer %lx\n",hr);
  IMediaControl*mc; IMediaEvent*me; IGraphBuilder_QueryInterface(gb,&IID_IMediaControl,(void**)&mc); IGraphBuilder_QueryInterface(gb,&IID_IMediaEvent,(void**)&me);
  hr=IMediaControl_Run(mc); printf("run %lx\n",hr); long ec; hr=IMediaEvent_WaitForCompletion(me,60000,&ec); printf("wait %lx ec=%ld\n",hr,ec); IMediaControl_Stop(mc); printf("done\n"); return 0;}
