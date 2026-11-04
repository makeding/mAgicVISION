/* Real local IPC and owner worker, fake USB backend. No WinUSB linkage. */
#include "client.h"
#include "ipc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
struct xz_host { DWORD load,tunes,stops; BOOL streaming; };
static DWORD loads;
int xz_driver_host_main(void);
static HANDLE test_event(const wchar_t *suffix)
{
    wchar_t a[128],b[128],name[160],mutex[128];
    if(xz_ipc_names(a,b,mutex)) return NULL;
    swprintf(name,160,L"%ls.%ls",mutex,suffix);
    return CreateEventW(NULL,TRUE,FALSE,name);
}
static HANDLE restore_event(void) { return test_event(L"restored"); }
int xz_connect(struct xz_host **out,const wchar_t *fw,const wchar_t *snapshot,const wchar_t *log)
{
    (void)fw; (void)snapshot; (void)log;
    *out=calloc(1,sizeof(**out));
    if(!*out) return -ENOMEM;
    (*out)->load=++loads; return 0;
}
int xz_prepare(struct xz_host *h,const struct xz_operation *operation)
{
    (void)h;
    for(unsigned i=0;i<100;++i) {
        if(WaitForSingleObject(operation->cancel,10)==WAIT_OBJECT_0) return -ECANCELED;
    }
    return 0;
}
int xz_tune(struct xz_host *h,const struct mb86H_dvb_request *r,const struct xz_operation *op)
{
    if(r->frequency==485142857) {
        while(WaitForSingleObject(op->cancel,10)==WAIT_TIMEOUT)
            if(GetTickCount64()>=op->deadline) return -ETIMEDOUT;
        return -ECANCELED;
    }
    Sleep(50);
    h->streaming=TRUE; ++h->tunes; return 0;
}
const char *xz_failure_stage(struct xz_host *h) { (void)h; return "RF acquisition"; }
int xz_detach(struct xz_host *h,const struct xz_operation *op)
{ (void)op; if(!h) return -EINVAL; h->streaming=FALSE; ++h->stops; return 0; }
int xz_close(struct xz_host **h)
{
    free(*h); *h=NULL;
    HANDLE event=restore_event();
    if(event) { SetEvent(event); CloseHandle(event); }
    return 0;
}
DWORD xz_wait(struct xz_host *h,DWORD timeout)
{ (void)timeout; return h->streaming?WAIT_OBJECT_0:WAIT_ABANDONED; }
DWORD xz_ready(struct xz_host *h) { return h->streaming?1:0; }
int xz_read(struct xz_host *h,BYTE *data,DWORD capacity,DWORD *size,DWORD *remain)
{
    if(!h->streaming || capacity<188) return -EIO;
    memset(data,0xff,188); data[0]=0x47;
    data[1]=(BYTE)h->load; data[2]=(BYTE)h->tunes; data[3]=(BYTE)h->stops;
    *size=188; if(remain) *remain=0; return 0;
}
void xz_purge(struct xz_host *h) { (void)h; }
static int settled(struct xz_client *c,DWORD phase)
{
    ULONGLONG end=GetTickCount64()+3000;
    do {
        struct xz_ipc_state s;
        if(xz_client_state(c,&s) || s.error) return 1;
        if(s.phase==phase) return 0;
        Sleep(10);
    } while(GetTickCount64()<end);
    return 1;
}
static int check_stream(struct xz_client *c,BYTE tunes)
{
    BYTE data[XZ_IPC_TS_BYTES]; DWORD size=0,remain=0;
    return xz_client_ready(c)!=1 || xz_client_wait(c,0)!=WAIT_OBJECT_0 ||
       xz_client_read(c,data,sizeof(data),&size,&remain) || size!=188 || remain ||
       data[0]!=0x47 || data[1]!=1 || data[2]!=tunes;
}
static int caller_test(const wchar_t *exe,BOOL abrupt)
{
    struct xz_client *a=NULL,*b=NULL;
    const wchar_t *log=L"host-ipc-probe.log";
    ULONGLONG start=GetTickCount64();
    int rc=xz_client_open(&a,exe,L"mock",log,NULL);
    if(rc || GetTickCount64()-start>700) return 2;
    if(abrupt) goto caller_exit;
    struct xz_ipc_state state;
    if(xz_client_state(a,&state) || state.phase!=XZ_IPC_PREPARING ||
       state.selected || state.completed || xz_client_wait(a,0)!=WAIT_TIMEOUT) return 3;
    struct mb86H_dvb_request r={.system=MB86H_DVB_GR,.frequency=473142857,.bandwidth_hz=6000000,
        .stream_id=UINT32_MAX,.layers=7};
    start=GetTickCount64();
    if(xz_client_tune(a,&r) || GetTickCount64()-start>300) return 4;
    r.frequency+=6000000;
    if(xz_client_tune(a,&r) || settled(a,XZ_IPC_STREAMING) || check_stream(a,1)) return 5;
    rc=xz_client_open(&b,exe,L"mock",log,NULL);
    xz_client_close(&b);
    if(rc!=-EBUSY) { printf("terminal BUSY error lost rc=%d\n",rc); return 6; }
    if(xz_client_close(&a)) return 7;
    /* Wait for confirmed cleanup, while validating BUSY terminal delivery. */
    ULONGLONG deadline=GetTickCount64()+2000;
    do {
        rc=xz_client_open(&a,exe,L"mock",log,NULL);
        if(!rc) break;
        xz_client_close(&a);
        if(rc!=-EBUSY) return 8;
        Sleep(10);
    } while(GetTickCount64()<deadline);
    if(rc || settled(a,XZ_IPC_IDLE) || xz_client_tune(a,&r) ||
       settled(a,XZ_IPC_STREAMING) || check_stream(a,2)) return 9;
    r.frequency=485142857;
    if(xz_client_tune(a,&r) || settled(a,XZ_IPC_TUNING)) return 10;
    start=GetTickCount64();
    if(xz_client_close(&a) || GetTickCount64()-start>300) return 11;
    Sleep(100);
    rc=xz_client_open(&b,exe,L"mock",log,NULL);
    xz_client_close(&b);
    if(rc!=-ECANCELED) { printf("terminal cancellation error lost rc=%d\n",rc); return 12; }
caller_exit:
    /* Keep the caller alive until the parent holds the actual server process
     * handle; then exit without Close in the abrupt preparation case. */
    HANDLE ready=test_event(L"caller-ready"),release=test_event(L"caller-release");
    if(!ready || !release) return 13;
    SetEvent(ready);
    DWORD wait=WaitForSingleObject(release,10000);
    CloseHandle(ready); CloseHandle(release);
    if(wait!=WAIT_OBJECT_0) return 14;
    return 0;
}
static int process_test(const wchar_t *exe,BOOL abrupt)
{
    HANDLE restoration=restore_event(),ready=test_event(L"caller-ready"),
        release=test_event(L"caller-release"),server=NULL;
    PROCESS_INFORMATION child={0}; STARTUPINFOW startup={.cb=sizeof(startup)};
    int rc=20; DWORD code=0;
    if(!restoration || !ready || !release) goto done;
    ResetEvent(restoration); ResetEvent(ready); ResetEvent(release);
    wchar_t command[32768];
    if(swprintf(command,32768,L"\"%ls\" --caller-%ls",exe,abrupt?L"abrupt":L"normal")<0) goto done;
    if(!CreateProcessW(exe,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&child)) goto done;
    HANDLE events[2]={ready,child.hProcess};
    if(WaitForMultipleObjects(2,events,FALSE,10000)!=WAIT_OBJECT_0) { rc=21; goto done; }
    wchar_t control[128],data[128],mutex[128];
    if(xz_ipc_names(control,data,mutex)) goto done;
    HANDLE pipe=CreateFileW(control,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
    DWORD pid=0;
    if(pipe==INVALID_HANDLE_VALUE) { rc=22; goto done; }
    if(GetNamedPipeServerProcessId(pipe,&pid)) server=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    CloseHandle(pipe);
    if(!server || WaitForSingleObject(restoration,0)!=WAIT_TIMEOUT) { rc=23; goto done; }
    SetEvent(release);
    if(WaitForSingleObject(child.hProcess,5000)!=WAIT_OBJECT_0 ||
       !GetExitCodeProcess(child.hProcess,&code) || code) { rc=24; goto done; }
    if(WaitForSingleObject(restoration,5000)!=WAIT_OBJECT_0) { rc=25; goto done; }
    if(WaitForSingleObject(server,5000)!=WAIT_OBJECT_0 ||
       !GetExitCodeProcess(server,&code) || code) { rc=26; goto done; }
    rc=0;
done:
    if(release) SetEvent(release);
    if(child.hProcess) { WaitForSingleObject(child.hProcess,10000); CloseHandle(child.hProcess); }
    if(child.hThread) CloseHandle(child.hThread);
    if(server) CloseHandle(server);
    if(restoration) CloseHandle(restoration);
    if(ready) CloseHandle(ready);
    if(release) CloseHandle(release);
    return rc;
}
int wmain(int argc,wchar_t **argv)
{
    if(argc==1) return xz_driver_host_main();
    wchar_t exe[32768];
    if(!GetModuleFileNameW(NULL,exe,32768)) return 2;
    if(argc==2 && !wcscmp(argv[1],L"--caller-normal")) return caller_test(exe,FALSE);
    if(argc==2 && !wcscmp(argv[1],L"--caller-abrupt")) return caller_test(exe,TRUE);
    if(argc!=2 || wcscmp(argv[1],L"--test")) return 2;
    int rc=process_test(exe,FALSE);
    if(!rc) rc=process_test(exe,TRUE);
    if(rc) printf("FAIL async owner step=%d win32=%lu\n",rc,GetLastError());
    else puts("PASS: responsive open, queued tune, generation TS, BUSY/failure delivery, warm reuse, cancellation, restore and helper exit after normal/abrupt caller exit (no USB)");
    return rc;
}
