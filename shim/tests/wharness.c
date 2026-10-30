/* SPDX-License-Identifier: GPL-3.0-or-later */
#define COBJMACROS
#define STRSAFE_NO_DEPRECATE
#include <windows.h>
#include <dshow.h>
#include <wmsdk.h>
#include <stdio.h>
static const GUID CLSID_Reader={0x187463a0,0x5bb7,0x11d3,{0xac,0xbe,0x00,0x80,0xc7,0x5e,0x24,0x6e}};
static const GUID CLSID_Writer={0x7c23220e,0x55bb,0x11d3,{0x8b,0x16,0x00,0xc0,0x4f,0xb6,0xbd,0x3d}};
static const GUID IID_ICfg={0x45086030,0xf7e4,0x486a,{0xb5,0x04,0x82,0x6b,0xb5,0x79,0x2a,0x3b}};
typedef HRESULT (WINAPI *GCO)(REFCLSID,REFIID,void**);
typedef HRESULT (WINAPI *PMC)(IWMProfileManager**);
static IPin* nth(IBaseFilter*f,int i,PIN_DIRECTION want){IEnumPins*e;IPin*p;int k=0;IBaseFilter_EnumPins(f,&e);while(IEnumPins_Next(e,1,&p,0)==S_OK){PIN_DIRECTION d;IPin_QueryDirection(p,&d);if(d==want){if(k==i){IEnumPins_Release(e);return p;}k++;}IPin_Release(p);}IEnumPins_Release(e);return 0;}
const char *PRX=
"<profile version=\"589824\" storageformat=\"1\" name=\"Test\" description=\"t\">"
"<streamconfig majortype=\"{73647561-0000-0010-8000-00AA00389B71}\" streamnumber=\"1\" streamname=\"Audio Stream\" inputname=\"Audio409\" bitrate=\"96000\" bufferwindow=\"3000\">"
"<wmmediatype subtype=\"{00000161-0000-0010-8000-00AA00389B71}\" bfixedsizesamples=\"1\" btemporalcompression=\"0\" lsamplesize=\"4459\"><waveformatex wformattag=\"353\" nchannels=\"2\" nsamplespersec=\"44100\" navgbytespersec=\"12000\" nblockalign=\"4459\" wbitspersample=\"16\" codecdata=\"008800000F0000\"/></wmmediatype></streamconfig>"
"<streamconfig majortype=\"{73646976-0000-0010-8000-00AA00389B71}\" streamnumber=\"2\" streamname=\"Video Stream\" inputname=\"Video409\" bitrate=\"800000\" bufferwindow=\"5000\">"
"<videomediaprops maxkeyframespacing=\"80000000\" quality=\"90\"/>"
"<wmmediatype subtype=\"{33564D57-0000-0010-8000-00AA00389B71}\" bfixedsizesamples=\"0\" btemporalcompression=\"1\" lsamplesize=\"0\"><videoinfoheader dwbitrate=\"800000\" dwbiterrorrate=\"0\" avgtimeperframe=\"400000\"><rcsource left=\"0\" top=\"0\" right=\"320\" bottom=\"240\"/><rctarget left=\"0\" top=\"0\" right=\"320\" bottom=\"240\"/><bitmapinfoheader biwidth=\"320\" biheight=\"240\" biplanes=\"1\" bibitcount=\"24\" bicompression=\"WMV3\" bisizeimage=\"0\"/></videoinfoheader></wmmediatype></streamconfig></profile>";
int main(int argc,char**argv){
  CoInitialize(0);
  HMODULE m=LoadLibraryA("asfreader.dll"); GCO g=(GCO)GetProcAddress(m,"DllGetClassObject");
  HMODULE w=LoadLibraryA("wmvcore.dll"); PMC pmc=(PMC)GetProcAddress(w,"WMCreateProfileManager");
  IClassFactory*cf; IBaseFilter*rd,*wr;
  g(&CLSID_Reader,&IID_IClassFactory,(void**)&cf); IClassFactory_CreateInstance(cf,0,&IID_IBaseFilter,(void**)&rd);
  g(&CLSID_Writer,&IID_IClassFactory,(void**)&cf); HRESULT hr=IClassFactory_CreateInstance(cf,0,&IID_IBaseFilter,(void**)&wr); printf("writer %lx\n",hr);
  IGraphBuilder*gb; CoCreateInstance(&CLSID_FilterGraph,0,CLSCTX_INPROC_SERVER,&IID_IGraphBuilder,(void**)&gb);
  IGraphBuilder_AddFilter(gb,rd,L"rd"); IGraphBuilder_AddFilter(gb,wr,L"wr");
  IFileSinkFilter*sink; IBaseFilter_QueryInterface(wr,&IID_IFileSinkFilter,(void**)&sink); WCHAR of[260]; MultiByteToWideChar(CP_ACP,0,argv[2],-1,of,260); hr=IFileSinkFilter_SetFileName(sink,of,0); printf("setfile %lx\n",hr);
  IWMProfileManager*pm; pmc(&pm); IWMProfile*prof; WCHAR wprx[8192]; MultiByteToWideChar(CP_UTF8,0,PRX,-1,wprx,8192); hr=IWMProfileManager_LoadProfileByData(pm,wprx,&prof); printf("loadprofile %lx\n",hr);
  struct ICfg{const struct V{HRESULT(WINAPI*q)(void*,REFIID,void**);ULONG(WINAPI*a)(void*);ULONG(WINAPI*r)(void*);HRESULT(WINAPI*c1)(void*,DWORD);HRESULT(WINAPI*c2)(void*,DWORD*);HRESULT(WINAPI*c3)(void*,REFGUID);HRESULT(WINAPI*c4)(void*,GUID*);HRESULT(WINAPI*c5)(void*,IWMProfile*);} *v;} *cfg;
  hr=IBaseFilter_QueryInterface(wr,&IID_ICfg,(void**)&cfg); printf("qi cfg %lx\n",hr); hr=cfg->v->c5(cfg,prof); printf("configure %lx\n",hr);
  IFileSourceFilter*fs; IBaseFilter_QueryInterface(rd,&IID_IFileSourceFilter,(void**)&fs); WCHAR wf[512]; MultiByteToWideChar(CP_ACP,0,argv[1],-1,wf,512); hr=IFileSourceFilter_Load(fs,wf,0); printf("load %lx\n",hr);
  for(int i=0;i<2;i++){ IPin*o=nth(rd,i,PINDIR_OUTPUT); IPin*in=nth(wr,i,PINDIR_INPUT); IPin*in2=nth(wr,1-i,PINDIR_INPUT);
    hr=IGraphBuilder_Connect(gb,o,in); if(FAILED(hr)) hr=IGraphBuilder_Connect(gb,o,in2); printf("connect %d %lx\n",i,hr);}
  IMediaControl*mc; IMediaEvent*me; IGraphBuilder_QueryInterface(gb,&IID_IMediaControl,(void**)&mc); IGraphBuilder_QueryInterface(gb,&IID_IMediaEvent,(void**)&me);
  hr=IMediaControl_Run(mc); printf("run %lx\n",hr);
  long ec; hr=IMediaEvent_WaitForCompletion(me,60000,&ec); printf("wait %lx ec=%ld\n",hr,ec);
  hr=IMediaControl_Stop(mc); printf("stop %lx\n",hr);
  printf("done\n"); return 0;}
