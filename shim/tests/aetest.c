/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Audio Enc pass-through -> ASF writer without any graph: a fake source pushes mono 44.1k sine PCM like the game's WAV parser would.
 * usage: aetest.exe out.wmv profile.xml   (manifest must register SystemDeviceEnum + ASF writer CLSIDs to asfreader.dll) */
#define COBJMACROS
#include <windows.h>
#include <dshow.h>
#include <wmsdk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static const GUID CLSID_Writer={0x7c23220e,0x55bb,0x11d3,{0x8b,0x16,0x00,0xc0,0x4f,0xb6,0xbd,0x3d}};
static const GUID CLSID_SDE={0x62BE5D10,0x60EB,0x11d0,{0xBD,0x3B,0x00,0xA0,0xC9,0x11,0xCE,0x86}};
static const GUID CAT_A={0x33D9A761,0x90C8,0x11d0,{0xBD,0x43,0x00,0xA0,0xC9,0x11,0xCE,0x86}};
static const GUID IID_ICfg={0x45086030,0xf7e4,0x486a,{0xb5,0x04,0x82,0x6b,0xb5,0x79,0x2a,0x3b}};
static const GUID CLSID_MemAlloc={0x1e651cc0,0xb199,0x11d0,{0x82,0x12,0x00,0xc0,0x4f,0xc3,0x2c,0x45}};
typedef HRESULT (WINAPI *PMC)(IWMProfileManager**);
static IPin* nth(IBaseFilter*f,int i,PIN_DIRECTION want){IEnumPins*e;IPin*p;int k=0;IBaseFilter_EnumPins(f,&e);while(IEnumPins_Next(e,1,&p,0)==S_OK){PIN_DIRECTION d;IPin_QueryDirection(p,&d);if(d==want){if(k==i){IEnumPins_Release(e);return p;}k++;}IPin_Release(p);}IEnumPins_Release(e);return 0;}
/* fake upstream pin: only needs to survive AddRef/Release */
static HRESULT WINAPI fq(IPin*t,REFIID r,void**o){*o=t;return S_OK;} static ULONG WINAPI fa(IPin*t){return 2;}
static IPinVtbl fvt; static IPin fakepin={&fvt};
int main(int argc,char**argv){
  CoInitialize(0); HRESULT hr; WCHAR w[512];
  fvt.QueryInterface=fq; fvt.AddRef=fa; fvt.Release=(void*)fa;
  IBaseFilter *ae=0,*wr; ICreateDevEnum*de; IEnumMoniker*em; IMoniker*m;
  CoCreateInstance(&CLSID_SDE,0,1,&IID_ICreateDevEnum,(void**)&de); ICreateDevEnum_CreateClassEnumerator(de,&CAT_A,&em,0); IEnumMoniker_Next(em,1,&m,0); IMoniker_BindToObject(m,0,0,&IID_IBaseFilter,(void**)&ae); printf("audio enc %p\n",ae);
  CoCreateInstance(&CLSID_Writer,0,1,&IID_IBaseFilter,(void**)&wr);
  IFileSinkFilter*sink; IBaseFilter_QueryInterface(wr,&IID_IFileSinkFilter,(void**)&sink); MultiByteToWideChar(CP_ACP,0,argv[1],-1,w,512); IFileSinkFilter_SetFileName(sink,w,0);
  FILE*f=fopen(argv[2],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); char*x=malloc(n+1); fread(x,1,n,f); x[n]=0; WCHAR*wx=malloc((n+1)*2); MultiByteToWideChar(CP_UTF8,0,x,-1,wx,n+1);
  PMC pmc=(PMC)GetProcAddress(LoadLibraryA("wmvcore.dll"),"WMCreateProfileManager"); IWMProfileManager*pm; pmc(&pm); IWMProfile*prof; IWMProfileManager_LoadProfileByData(pm,wx,&prof);
  struct ICfg{const struct V{void*q,*a,*r,*c1,*c2,*c3,*c4; HRESULT(WINAPI*c5)(void*,IWMProfile*);} *v;} *cfg; IBaseFilter_QueryInterface(wr,&IID_ICfg,(void**)&cfg); cfg->v->c5(cfg,prof);
  WAVEFORMATEX wf={WAVE_FORMAT_PCM,1,44100,88200,2,16,0}; AM_MEDIA_TYPE mt={0}; mt.majortype=MEDIATYPE_Audio; mt.subtype=MEDIASUBTYPE_PCM; mt.formattype=FORMAT_WaveFormatEx; mt.cbFormat=sizeof wf; mt.pbFormat=(BYTE*)&wf;
  IPin*ain=nth(ae,0,PINDIR_INPUT),*aout=nth(ae,0,PINDIR_OUTPUT),*win=nth(wr,0,PINDIR_INPUT);
  hr=IPin_ReceiveConnection(ain,&fakepin,&mt); printf("enc in recvconn %lx\n",hr);
  hr=IPin_Connect(aout,win,NULL); printf("enc out -> writer %lx\n",hr);
  IMemInputPin*mip; IPin_QueryInterface(ain,&IID_IMemInputPin,(void**)&mip); IMemAllocator*al; hr=IMemInputPin_GetAllocator(mip,&al); printf("alloc %lx\n",hr);
  ALLOCATOR_PROPERTIES rq={4,16384,1,0},ac; IMemAllocator_SetProperties(al,&rq,&ac); IMemAllocator_Commit(al); IMemInputPin_NotifyAllocator(mip,al,FALSE);
  hr=IBaseFilter_Pause(wr); IBaseFilter_Pause(ae); IBaseFilter_Run(wr,0); IBaseFilter_Run(ae,0); printf("run %lx\n",hr);
  int total=44100*6, done=0; double ph=0; while(done<total){ IMediaSample*s; hr=IMemAllocator_GetBuffer(al,&s,0,0,0); if(FAILED(hr)){printf("getbuffer %lx\n",hr);break;}
    BYTE*b; IMediaSample_GetPointer(s,&b); int k=8192; if(done+k>total)k=total-done; short*p=(short*)b; for(int i=0;i<k;i++){p[i]=(short)(12000*sin(ph)); ph+=2*3.14159265*440/44100;}
    IMediaSample_SetActualDataLength(s,k*2); hr=IMemInputPin_Receive(mip,s); IMediaSample_Release(s); if(hr!=S_OK){printf("receive %lx\n",hr);break;} done+=k; }
  hr=IPin_EndOfStream(ain); printf("eos %lx (samples %d)\n",hr,done);
  IBaseFilter_Stop(ae); IBaseFilter_Stop(wr); printf("done\n"); return 0;}
