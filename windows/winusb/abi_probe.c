/* Hardware-free test of the actual DLL with Microsoft's RTTI runtime. */
#include "bon_abi.h"
#include <stdio.h>
#include <string.h>
struct object { const struct bon_vtable *vtable; };
struct type_descriptor { void *vtable,*spare; char name[24]; };
int bon_msvc_consumer(void *);
int main(void)
{
    HMODULE dll=LoadLibraryW(L"BonDriver_mb86H.dll");
    if(!dll) { printf("LoadLibrary win32=%lu\n",GetLastError()); return 1; }
    typedef struct bon *(*create_type)(void);
    create_type create=NULL;
    FARPROC entry=GetProcAddress(dll,"CreateBonDriver");
    memcpy(&create,&entry,sizeof(create));
    if(!create) return 2;
    struct bon *bon=create();
    if(!bon) { puts("CreateBonDriver failed"); return 3; }
    struct object *object=(struct object *)bon;
    HMODULE runtime=GetModuleHandleW(L"msvcrt.dll");
    typedef void *(*cast_type)(void *,long,void *,void *,int);
    cast_type cast=NULL;
    entry=GetProcAddress(runtime,"__RTDynamicCast"); memcpy(&cast,&entry,sizeof(cast));
    if(!cast) { puts("__RTDynamicCast missing"); return 4; }
    struct type_descriptor base={0},derived={0},foreign={0};
    strcpy(base.name,".?AVIBonDriver@@"); strcpy(derived.name,".?AVIBonDriver2@@");
    strcpy(foreign.name,".?AVIBonDriver3@@");
    if(cast(bon,0,&base,&derived,0)!=bon || cast(bon,0,&base,&foreign,0)!=NULL) {
        puts("MSVC dynamic_cast failed"); return 5;
    }
    int checked=bon_msvc_consumer(bon);
    if(checked) { printf("compiler-generated MSVC consumer failed=%d\n",checked); return 7; }
    const struct bon_vtable *v=object->vtable;
    DWORD size=99,remain=99; BYTE *pointer=NULL,buffer[256*188];
    if(wcscmp(v->name(bon),L"I-O DATA GV-MVP/XZ ISDB-T/S Tuner") ||
       !v->space(bon,0) || !v->channel_name(bon,0,39) ||
       v->channel_name(bon,0,40) || v->current_space(bon)!=0xffffffff ||
       v->current_channel(bon)!=0xffffffff || v->opening(bon) ||
       v->wait(bon,0)!=WAIT_ABANDONED || v->ready(bon) ||
       v->stream_pointer(bon,&pointer,&size,&remain) || size || remain || !pointer ||
       v->stream_copy(bon,buffer,&size,&remain) || size || remain) {
        puts("BonDriver vtable/closed-state checks failed"); return 6;
    }
    v->purge(bon); v->close(bon); v->release(bon);
    FreeLibrary(dll);
    puts("PASS: CreateBonDriver, MSVC dynamic_cast, all non-device IBonDriver2 calls");
    return 0;
}
