/* SPDX-License-Identifier: GPL-3.0-or-later */
#define COBJMACROS
#define STRSAFE_NO_DEPRECATE
#include <windows.h>
#include <dshow.h>
#include <stdio.h>
static const GUID CLSID_Reader={0x187463a0,0x5bb7,0x11d3,{0xac,0xbe,0x00,0x80,0xc7,0x5e,0x24,0x6e}};
static const GUID CLSID_Null={0xc1f400a4,0x3f08,0x11d3,{0x9f,0x0b,0x00,0x60,0x08,0x03,0x9e,0x37}};
typedef HRESULT (WINAPI *GCO)(REFCLSID,REFIID,void**);
static IPin* nth(IBaseFilter*f,int i,PIN_DIRECTION want){IEnumPins*e;IPin*p;int k=0;IBaseFilter_EnumPins(f,&e);while(IEnumPins_Next(e,1,&p,0)==S_OK){PIN_DIRECTION d;IPin_QueryDirection(p,&d);if(d==want){if(k==i){IEnumPins_Release(e);return p;}k++;}IPin_Release(p);}IEnumPins_Release(e);return 0;}
int main(int argc,char**argv){
  CoInitialize(0);
  HMODULE m=LoadLibraryA("asfreader.dll"); GCO g=(GCO)GetProcAddress(m,"DllGetClassObject");
  IClassFactory*cf; HRESULT hr=g(&CLSID_Reader,&IID_IClassFactory,(void**)&cf); printf("gco %lx\n",hr);
  IBaseFilter*rd; hr=IClassFactory_CreateInstance(cf,0,&IID_IBaseFilter,(void**)&rd); printf("create %lx\n",hr);
  IGraphBuilder*gb; hr=CoCreateInstance(&CLSID_FilterGraph,0,CLSCTX_INPROC_SERVER,&IID_IGraphBuilder,(void**)&gb); printf("graph %lx\n",hr);
  hr=IGraphBuilder_AddFilter(gb,rd,L"Asf Reader"); printf("addfilter %lx\n",hr);
  IFileSourceFilter*fs; IBaseFilter_QueryInterface(rd,&IID_IFileSourceFilter,(void**)&fs);
  WCHAR wf[512]; MultiByteToWideChar(CP_ACP,0,argv[1],-1,wf,512);
  hr=IFileSourceFilter_Load(fs,wf,0); printf("load %lx\n",hr);
  for(int i=0;i<2;i++){ IBaseFilter*nr; hr=CoCreateInstance(&CLSID_Null,0,CLSCTX_INPROC_SERVER,&IID_IBaseFilter,(void**)&nr); if(FAILED(hr)){printf("null renderer %lx\n",hr);return 1;} IGraphBuilder_AddFilter(gb,nr,L"nr");
    IPin*o=nth(rd,i,PINDIR_OUTPUT), *in=nth(nr,0,PINDIR_INPUT); hr=IGraphBuilder_Connect(gb,o,in); printf("connect %d %lx\n",i,hr);}
  IMediaControl*mc; IMediaEvent*me; IMediaSeeking*ms; IGraphBuilder_QueryInterface(gb,&IID_IMediaControl,(void**)&mc); IGraphBuilder_QueryInterface(gb,&IID_IMediaEvent,(void**)&me); IGraphBuilder_QueryInterface(gb,&IID_IMediaSeeking,(void**)&ms);
  LONGLONG d=0; hr=IMediaSeeking_GetDuration(ms,&d); printf("duration %lx %lld\n",hr,d);
  hr=IMediaControl_Run(mc); printf("run %lx\n",hr);
  long ec; hr=IMediaEvent_WaitForCompletion(me,20000,&ec); printf("wait1 %lx ec=%ld\n",hr,ec);
  LONGLONG pos=40000000, stop=0; hr=IMediaSeeking_SetPositions(ms,&pos,AM_SEEKING_AbsolutePositioning,&stop,AM_SEEKING_NoPositioning); printf("seek %lx\n",hr);
  hr=IMediaEvent_WaitForCompletion(me,20000,&ec); printf("wait2 %lx ec=%ld\n",hr,ec);
  pos=0; hr=IMediaSeeking_SetPositions(ms,&pos,AM_SEEKING_AbsolutePositioning,&stop,AM_SEEKING_NoPositioning); printf("seek0 %lx\n",hr);
  Sleep(300); hr=IMediaControl_Stop(mc); printf("stop %lx\n",hr);
  hr=IMediaControl_Run(mc); Sleep(300); hr=IMediaControl_Stop(mc); printf("rerun/stop %lx\n",hr);
  printf("done\n"); return 0;}
