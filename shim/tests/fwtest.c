/* SPDX-License-Identifier: GPL-3.0-or-later */
/* File Writer test: sequential Receive() data, then IStream Seek(0)+Write() header, like the game's Wave Dest. Run next to asfreader.dll. */
#define COBJMACROS
#include <windows.h>
#include <dshow.h>
#include <stdio.h>
static const GUID CLSID_FW={0x8596e5f0,0x0da5,0x11d0,{0xbd,0x21,0x00,0xa0,0xc9,0x11,0xce,0x86}};
typedef HRESULT (WINAPI *GCO)(REFCLSID,REFIID,void**);
typedef struct { const IMediaSampleVtbl *v; BYTE *buf; LONG len; } MS;
static HRESULT WINAPI q(IMediaSample*t,REFIID r,void**o){*o=t;return S_OK;} static ULONG WINAPI a(IMediaSample*t){return 2;} static ULONG WINAPI rl(IMediaSample*t){return 1;}
static HRESULT WINAPI gp(IMediaSample*t,BYTE**p){*p=((MS*)t)->buf;return S_OK;} static LONG WINAPI gs(IMediaSample*t){return ((MS*)t)->len;} static LONG WINAPI gal(IMediaSample*t){return ((MS*)t)->len;}
static HRESULT WINAPI nop(IMediaSample*t){return E_NOTIMPL;}
int main(int argc,char**argv){
  CoInitialize(0); HMODULE m=LoadLibraryA("asfreader.dll"); GCO g=(GCO)GetProcAddress(m,"DllGetClassObject");
  IClassFactory*cf; IBaseFilter*f; HRESULT hr=g(&CLSID_FW,&IID_IClassFactory,(void**)&cf); printf("gco %lx\n",hr);
  hr=IClassFactory_CreateInstance(cf,0,&IID_IBaseFilter,(void**)&f); printf("create %lx\n",hr);
  IFileSinkFilter*sink; IBaseFilter_QueryInterface(f,&IID_IFileSinkFilter,(void**)&sink); WCHAR fn[260]; MultiByteToWideChar(CP_ACP,0,argv[1],-1,fn,260); IFileSinkFilter_SetFileName(sink,fn,0);
  IEnumPins*e; IPin*pin; IBaseFilter_EnumPins(f,&e); IEnumPins_Next(e,1,&pin,0);
  IStream*st; hr=IPin_QueryInterface(pin,&IID_IStream,(void**)&st); printf("QI IStream %lx\n",hr);
  IMemInputPin*mip; IPin_QueryInterface(pin,&IID_IMemInputPin,(void**)&mip);
  IBaseFilter_Pause(f);
  BYTE hdr[46]; memset(hdr,0,46);            /* WavDest reserves the header with a blank sample */
  BYTE data[4000]; for(int i=0;i<4000;i++) data[i]=(BYTE)(i|1);
  IMediaSampleVtbl vt={q,a,rl,gp,gs,0,0,gal,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}; vt.GetActualDataLength=gs; vt.GetPointer=gp;
  MS s1={&vt,hdr,46}, s2={&vt,data,4000};
  hr=IMemInputPin_Receive(mip,(IMediaSample*)&s1); printf("recv hdr %lx\n",hr);
  hr=IMemInputPin_Receive(mip,(IMediaSample*)&s2); printf("recv data %lx\n",hr);
  IBaseFilter_Run(f,0); IBaseFilter_Stop(f);
  LARGE_INTEGER z={0}; hr=IStream_Seek(st,z,STREAM_SEEK_SET,NULL); printf("seek %lx\n",hr);
  BYTE real[46]; memcpy(real,"RIFF",4); memset(real+4,0x11,42); ULONG w=0; hr=IStream_Write(st,real,46,&w); printf("write %lx %lu\n",hr,w);
  IStream_Release(st); IMemInputPin_Release(mip); IPin_Release(pin); IBaseFilter_Release(f); printf("done\n"); return 0;}
