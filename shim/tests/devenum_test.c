/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Lists FriendlyName of every filter in a DirectShow category (default: audio compressors). */
#define COBJMACROS
#include <windows.h>
#include <dshow.h>
#include <stdio.h>
static const GUID CLSID_SysDevEnum={0x62BE5D10,0x60EB,0x11d0,{0xBD,0x3B,0x00,0xA0,0xC9,0x11,0xCE,0x86}};
static const GUID CAT_Audio={0x33D9A761,0x90C8,0x11d0,{0xBD,0x43,0x00,0xA0,0xC9,0x11,0xCE,0x86}};
static const GUID CAT_Video={0x33D9A760,0x90C8,0x11d0,{0xBD,0x43,0x00,0xA0,0xC9,0x11,0xCE,0x86}};
int main(int argc,char**argv){
  CoInitialize(0); ICreateDevEnum*de; IEnumMoniker*em; HRESULT hr=CoCreateInstance(&CLSID_SysDevEnum,0,CLSCTX_INPROC_SERVER,&IID_ICreateDevEnum,(void**)&de); printf("devenum %lx\n",hr);
  hr=ICreateDevEnum_CreateClassEnumerator(de,argc>1?&CAT_Video:&CAT_Audio,&em,0); printf("category enum %lx\n",hr); if(hr!=S_OK) return 0;
  IMoniker*m; int n=0; while(IEnumMoniker_Next(em,1,&m,0)==S_OK){ IPropertyBag*pb; VARIANT v; VariantInit(&v);
    if(SUCCEEDED(IMoniker_BindToStorage(m,0,0,&IID_IPropertyBag,(void**)&pb))){ if(SUCCEEDED(IPropertyBag_Read(pb,L"FriendlyName",&v,0))) wprintf(L"  [%d] %ls\n",n,V_BSTR(&v)); IPropertyBag_Release(pb);} n++; }
  printf("%d entries\n",n); return 0;}
