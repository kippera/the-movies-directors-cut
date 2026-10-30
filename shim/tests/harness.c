/* SPDX-License-Identifier: GPL-3.0-or-later */
#define COBJMACROS
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <wmsdk.h>
#include <nserror.h>
const GUID IID_IWMHeaderInfo3={0x15cc68e3,0x27cc,0x4ecd,{0xb2,0x22,0x3f,0x5d,0x02,0xd8,0x0b,0xd5}};
typedef HRESULT (WINAPI *PF)(IUnknown*,DWORD,IWMSyncReader**);
int main(int argc,char**argv){
  HMODULE m=LoadLibraryA("wmvcore.dll"); PF f=(PF)GetProcAddress(m,"WMCreateSyncReader");
  CoInitialize(0);
  IWMSyncReader*r; HRESULT hr=f(0,1,&r); printf("create %lx\n",hr);
  IStream*s; hr=SHCreateStreamOnFileA(argv[1],STGM_READ,&s); printf("stream %lx\n",hr);
  hr=IWMSyncReader_OpenStream(r,s); printf("open %lx\n",hr);
  IWMHeaderInfo3*hi; hr=IWMSyncReader_QueryInterface(r,&IID_IWMHeaderInfo3,(void**)&hi); printf("hi %lx\n",hr);
  WMT_ATTR_DATATYPE ty; QWORD dur=0; WORD l=8,sn=0; hr=IWMHeaderInfo3_GetAttributeByName(hi,&sn,L"Duration",&ty,(BYTE*)&dur,&l); printf("dur %lx %llu\n",hr,dur);
  DWORD n; IWMSyncReader_GetOutputCount(r,&n); printf("outputs %lu\n",n);
  for(DWORD i=0;i<n;i++){ IWMOutputMediaProps*p; BYTE b[1024]; DWORD cb=sizeof b; IWMSyncReader_GetOutputProps(r,i,&p); IWMOutputMediaProps_GetMediaType(p,(WM_MEDIA_TYPE*)b,&cb); WM_MEDIA_TYPE*mt=(void*)b; printf(" out%lu cbFormat=%lu lSampleSize=%lu\n",i,mt->cbFormat,mt->lSampleSize);}
  int vcount=0,acount=0; unsigned long long abytes=0,lastts=0; int dumped=0;
  for(;;){ INSSBuffer*b; QWORD ts,d; DWORD fl,o; WORD st; hr=IWMSyncReader_GetNextSample(r,0,&b,&ts,&d,&fl,&o,&st); if(FAILED(hr)){printf("end %lx\n",hr);break;}
    BYTE*p; DWORD len; INSSBuffer_GetBufferAndLength(b,&p,&len);
    if(o==1){ vcount++; lastts=ts; if(vcount==60&&argc>2){ FILE*f=fopen(argv[2],"wb"); BITMAPFILEHEADER bf={0x4d42,54+len,0,0,54}; BITMAPINFOHEADER bi={40,640,480,1,32,0,len}; fwrite(&bf,14,1,f);fwrite(&bi,40,1,f);fwrite(p,len,1,f);fclose(f);} }
    else {acount++; abytes+=len;}
    INSSBuffer_Release(b);}
  printf("video frames %d last ts %llu; audio chunks %d bytes %llu\n",vcount,lastts,acount,abytes);
  hr=IWMSyncReader_SetRange(r,50000000,0); printf("setrange %lx\n",hr);
  INSSBuffer*b; QWORD ts,d; DWORD fl,o; WORD st; hr=IWMSyncReader_GetNextSample(r,0,&b,&ts,&d,&fl,&o,&st); printf("after seek: %lx out=%lu ts=%llu fl=%lx\n",hr,o,ts,fl);
  IWMSyncReader_Release(r); return 0;}
