#include "transport.h"
#include "binary_file.h"
#include "mb86H/usb/window.h"
#include "mb86H/protocol.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

static uint32_t now_ms(void *opaque) { (void)opaque; return (uint32_t)GetTickCount64(); }
static void delay_ms(void *opaque,unsigned ms) { (void)opaque; Sleep(ms); }
static void *allocate(void *opaque,size_t size) { (void)opaque; return malloc(size); }
static void release(void *opaque,void *p) { (void)opaque; free(p); }
static int native_control(void *opaque,const struct usb_window_control *c,void *data,unsigned timeout)
{
    struct xz_host *h=opaque;
    h->xz_read_cache.valid=false;
    return xz_control(h,c->request_type,c->value,c->index,data,c->length,timeout);
}
static const struct usb_native_ops native_ops={.control=native_control};
static int exchange(void *opaque,const uint8_t tx[64],uint8_t rx[64])
{
    struct xz_host *h=opaque;
    int rc=h->operation && h->operation->cancel &&
        WaitForSingleObject(h->operation->cancel,0)==WAIT_OBJECT_0 ? -ECANCELED :
        xz_operation_check(h,h->operation);
    if(rc) {
        xz_log(h,"command cancelled before exchange rc=%d operation=%u sequence=%u lease_cancel=%u",
            rc,((unsigned)tx[8]<<24)|((unsigned)tx[9]<<16)|((unsigned)tx[10]<<8)|tx[11],
            ((unsigned)tx[4]<<24)|((unsigned)tx[5]<<16)|((unsigned)tx[6]<<8)|tx[7],
            h->operation && h->operation->cancel &&
                WaitForSingleObject(h->operation->cancel,0)==WAIT_OBJECT_0);
        return rc;
    }
    /* Labels inspect shared packets only; the wire exchange remains unchanged. */
    uint32_t operation=((uint32_t)tx[8]<<24)|((uint32_t)tx[9]<<16)|((uint32_t)tx[10]<<8)|tx[11];
    uint32_t offset=((uint32_t)tx[12]<<24)|((uint32_t)tx[13]<<16)|((uint32_t)tx[14]<<8)|tx[15];
    uint32_t argument=((uint32_t)tx[16]<<24)|((uint32_t)tx[17]<<16)|((uint32_t)tx[18]<<8)|tx[19];
    uint32_t length=((uint32_t)tx[20]<<24)|((uint32_t)tx[21]<<16)|((uint32_t)tx[22]<<8)|tx[23];
    const char *before=h->progress_stage;
    if(before && !strstr(before,"restore")) {
        if(operation==MB86H_DEBUG_RESIDENT_DISPATCH && offset==MB86H_RESIDENT_COMMAND_INITIALIZE)
            h->progress_stage="initialize";
        else if(operation==MB86H_DEBUG_RESIDENT_BLOCK)
            h->progress_stage=h->loader.resident_attempted ?
                (argument?"resident-program":"resident-verify") :
                (h->loader.resident_saved?"resident-backup":"resident-adopt-verify");
    }
    ULONGLONG now=GetTickCount64();
    if(h->progress_stage && (before!=h->progress_stage || now-h->progress_updated>=2000)) {
        xz_log(h,"loader progress stage=%s offset=%u length=%u elapsed_ms=%llu controls=%llu",
            h->progress_stage,offset,length,now-h->operation_started,h->progress_controls);
        h->progress_updated=now;
    }
    return usb_xz_exchange_cached(&native_ops,h,&h->xz_read_cache,tx,rx);
}
static int read_window(void *opaque,uint32_t address,void *data,uint16_t size)
{
    struct xz_host *h=opaque;
    int rc=h->operation && h->operation->cancel &&
        WaitForSingleObject(h->operation->cancel,0)==WAIT_OBJECT_0 ? -ECANCELED :
        xz_operation_check(h,h->operation);
    if(rc) return rc;
    if(h->progress_stage && !strcmp(h->progress_stage,"loader") && !h->boot0_saved)
        h->progress_stage="boot0-backup";
    rc=usb_xz_window(&native_ops,h,address,data,size,false);
    if(!rc) xz_progress_window(h,"read",size);
    return rc;
}
static int snapshot_write(struct xz_host *h,const void *p,size_t size)
{ return fwrite(p,1,size,h->snapshot_file)==size ? 0 : -EIO; }
static int snapshot_flush(struct xz_host *h)
{ return fflush(h->snapshot_file) || _commit(_fileno(h->snapshot_file)) ? -EIO : 0; }
static int write_window(void *opaque,uint32_t address,const void *data,uint16_t size)
{
    struct xz_host *h=opaque;
    int rc=h->operation && h->operation->cancel &&
        WaitForSingleObject(h->operation->cancel,0)==WAIT_OBJECT_0 ? -ECANCELED :
        xz_operation_check(h,h->operation);
    if(rc) return rc;
    if(!h->boot0_saved) {
        /* Loader has read all of these before its first boot0 write. */
        const struct mb86H_loader *l=&h->loader;
        rc=snapshot_write(h,l->preserve_all_ram?"XZWINS01":"XZWINS02",8);
        if(!rc) rc=snapshot_write(h,l->hook,sizeof(l->hook));
        if(!rc) rc=snapshot_write(h,l->body,sizeof(l->body));
        if(!rc) rc=snapshot_write(h,l->ep0_hook,sizeof(l->ep0_hook));
        if(!rc) rc=snapshot_write(h,l->staging,sizeof(l->staging));
        if(!rc) rc=snapshot_flush(h);
        if(rc) return rc;
        h->boot0_saved=1;
        xz_log(h,"loader progress stage=boot0-preimage bytes=%u durable=true",
               (unsigned)(sizeof(l->hook)+sizeof(l->body)+sizeof(l->ep0_hook)+sizeof(l->staging)));
    }
    rc=usb_xz_window(&native_ops,h,address,(void *)data,size,true);
    if(!rc) xz_progress_window(h,"write",size);
    return rc;
}
static int snapshot(void *opaque,const struct mb86H_loader *l)
{
    struct xz_host *h=opaque;
    if (!l->preserve_all_ram) return snapshot_flush(h);
    int rc=snapshot_write(h,"RESIDENT",8);
    if(!rc) rc=snapshot_write(h,l->resident_saved,MB86H_RESIDENT_BYTES);
    if(!rc) rc=snapshot_flush(h);
    xz_log(h,"resident preimage persisted rc=%d",rc);
    if(!rc) h->progress_stage="resident-program";
    return rc;
}
static void mismatch(void *opaque,uint32_t address,uint32_t expected,uint32_t observed)
{
    struct xz_host *h=opaque;
    if(address>=MB86H_RESIDENT_COMMIT && address<MB86H_RESIDENT_COMMIT+4)
        h->failure_stage="resident commit marker (RSN1 missing)";
    xz_log(h,"mismatch address=%08x expected=%08x observed=%08x",address,expected,observed);
}
static void packet_hex(char out[129],const uint8_t *packet)
{
    static const char digits[]="0123456789abcdef";
    if(!packet) { strcpy(out,"unavailable"); return; }
    for(unsigned i=0;i<64;++i) {
        out[2*i]=digits[packet[i]>>4]; out[2*i+1]=digits[packet[i]&15];
    }
    out[128]=0;
}
static void observe(void *opaque,const uint8_t request[64],const uint8_t *response,int result)
{
    struct xz_host *h=opaque;
    uint32_t operation=mb86H_debug_u32be_get(request+8);
    if(operation!=MB86H_DEBUG_RESIDENT_DISPATCH) {
        if(!result) return;
    }
    uint32_t command=mb86H_debug_u32be_get(request+12);
    int32_t error0=0,error1=0;
    if(response && operation==MB86H_DEBUG_RESIDENT_DISPATCH &&
       (command==MB86H_RESIDENT_COMMAND_SERVICE || command==MB86H_RESIDENT_COMMAND_STATUS)) {
        error0=(int32_t)mb86H_debug_u32be_get(response+36);
        error1=(int32_t)mb86H_debug_u32be_get(response+40);
    }
    BOOL failed=result || (response && mb86H_debug_u32be_get(response+8)!=MB86H_DEBUG_OK) ||
        error0<0 || error1<0;
    if(!failed && (command==MB86H_RESIDENT_COMMAND_SERVICE ||
        command==MB86H_RESIDENT_COMMAND_STATUS)) return;
    char tx[129],rx[129]; packet_hex(tx,request); packet_hex(rx,response);
    xz_log(h,"FW reply stage=%s operation=%u command=%u result=%d error0=%d error1=%d request=%s response=%s",
        h->failure_stage?h->failure_stage:h->progress_stage?h->progress_stage:"service",
        operation,command,result,error0,error1,tx,rx);
}
static const struct mb86H_session_ops session_ops={.exchange=exchange,.observe=observe,.now_ms=now_ms,.delay_ms=delay_ms};
static struct mb86H_loader_ops loader_ops(struct xz_host *h)
{
    return (struct mb86H_loader_ops){.window_read=read_window,.window_write=write_window,
        .session=&h->session,.session_ops=&session_ops,.now_ms=now_ms,.delay_ms=delay_ms,
        .allocate=allocate,.release=release,.snapshot=snapshot,.mismatch=mismatch};
}
static void destroy(struct xz_host *h)
{
    xz_transport_close(h);
    if(h->stop) CloseHandle(h->stop);
    if(h->ready) CloseHandle(h->ready);
    if(h->snapshot_file) fclose(h->snapshot_file);
    if(h->log) fclose(h->log);
    free(h->storage); free(h->bundle);
    free(h->loader.resident_saved);
    DeleteCriticalSection(&h->queue_lock); DeleteCriticalSection(&h->log_lock);
    DeleteCriticalSection(&h->command_lock);
    free(h);
}
int xz_connect(struct xz_host **out,const wchar_t *bundle,const wchar_t *snapshot_path,const wchar_t *log)
{
    if(!out || !bundle || !snapshot_path || !log) return -EINVAL;
    *out=NULL;
    struct xz_host *h=calloc(1,sizeof(*h));
    if(!h) return -ENOMEM;
    h->file=INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&h->queue_lock); InitializeCriticalSection(&h->log_lock);
    InitializeCriticalSection(&h->command_lock);
    FILE *f=NULL;
    int rc=-EIO;
    const char *stage="log";
    h->log=_wfopen(log,L"a");
    if(!h->log) { xz_log(h,"log open path=%ls errno=%d",log,errno); goto done; }
    stage="snapshot";
    h->snapshot_file=xz_create_binary_file(snapshot_path);
    if(!h->snapshot_file) {
        xz_log(h,"snapshot create path=%ls errno=%d",snapshot_path,errno);
        goto done;
    }
    stage="stop event";
    h->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!h->stop) { xz_log(h,"stop event create win32=%lu",GetLastError()); goto done; }
    stage="ready event";
    h->ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!h->ready) { xz_log(h,"ready event create win32=%lu",GetLastError()); goto done; }
    stage="receive storage";
    h->storage=malloc(32768*MB86H_TS_PACKET_SIZE);
    if(!h->storage) { rc=-ENOMEM; goto done; }
    stage="firmware open";
    f=_wfopen(bundle,L"rb");
    if(!f) { xz_log(h,"firmware open path=%ls errno=%d",bundle,errno); goto done; }
    stage="firmware seek";
    if(fseek(f,0,SEEK_END)) { xz_log(h,"firmware seek errno=%d",errno); goto done; }
    long size=ftell(f);
    if(size<0) { xz_log(h,"firmware tell errno=%d",errno); goto done; }
    stage="firmware rewind";
    if(fseek(f,0,SEEK_SET)) { xz_log(h,"firmware rewind errno=%d",errno); goto done; }
    stage="firmware size";
    if(size<64 || size>1024*1024) { rc=-EINVAL; goto done; }
    stage="firmware storage";
    h->bundle=malloc((size_t)size);
    if(!h->bundle) { rc=-ENOMEM; goto done; }
    stage="firmware read";
    if(fread(h->bundle,1,(size_t)size,f)!=(size_t)size) {
        xz_log(h,"firmware read errno=%d",errno); goto done;
    }
    stage="firmware parse";
    rc=mb86H_loader_parse(&h->loader,h->bundle,(size_t)size);
    if(rc) goto done;
    xz_log(h,"XZ WinUSB x64 bundle=%ls bytes=%ld snapshot=%ls",bundle,size,snapshot_path);
    h->session.tune_timeout_ms=70000;
    mb86H_ts_receiver_core_init(&h->receiver,h->storage,32768);
    stage="WinUSB open";
    rc=xz_transport_open(h);

done:
    if(f) fclose(f);
    if(rc) { xz_log(h,"open failed stage=%s rc=%d",stage,rc); destroy(h); }
    else *out=h;
    return rc;
}
int xz_prepare(struct xz_host *h,const struct xz_operation *operation)
{
    struct mb86H_loader_ops ops=loader_ops(h);
    xz_operation_begin(h,operation,"loader");
    int rc=mb86H_loader_start(&h->loader,&ops,h);
    xz_operation_end(h);
    xz_log(h,"loader_start rc=%d adopted=%d",rc,h->loader.adopted);
    if(rc && operation && operation->failed) operation->failed(operation->opaque,rc);
    if(rc==-129 && !h->loader.adopted && !h->loader.hook_attempted &&
       !h->loader.body_attempted && !h->loader.resident_attempted) {
        xz_log(h,"resident rejected before adoption; no owned preimage, no device restoration performed");
    } else if(rc && !InterlockedCompareExchange(&h->transport_error,0,0)) {
        struct xz_operation restore={.deadline=GetTickCount64()+180000};
        xz_operation_begin(h,&restore,"open-failure-restore");
        int restored=mb86H_loader_restore(&h->loader,&ops,h);
        xz_operation_end(h);
        xz_log(h,"open failure restore rc=%d",restored);
    }
    return rc;
}
/* Same common SERVICE contract as Linux; this only schedules the FW owner.
 * RX must run separately while SERVICE makes EP81 output progress. */
static DWORD WINAPI service_thread(void *opaque)
{
    struct xz_host *h=opaque;
    while(WaitForSingleObject(h->stop,1)==WAIT_TIMEOUT) {
        EnterCriticalSection(&h->command_lock);
        int rc=0;
        if(WaitForSingleObject(h->stop,0)==WAIT_TIMEOUT && h->session.streaming)
            rc=mb86H_session_service(&h->session,&session_ops,h);
        LeaveCriticalSection(&h->command_lock);
        if(rc) {
            EnterCriticalSection(&h->queue_lock);
            mb86H_ts_receiver_core_receive(&h->receiver,rc,NULL,0);
            SetEvent(h->ready); LeaveCriticalSection(&h->queue_lock);
            xz_log(h,"capture SERVICE failed rc=%d",rc);
            SetEvent(h->stop); break;
        }
    }
    return 0;
}
int xz_tune(struct xz_host *h,const struct mb86H_dvb_request *request,
            const struct xz_operation *operation)
{
    if(!h || !request || (unsigned)request->system>=MB86H_CHANNEL_COUNT ||
       !mb86H_dvb_request_valid(&mb86H_frontend_caps[request->system],request)) return -EINVAL;
    /* Retire the old threads before storing the caller-owned operation: the
     * SERVICE worker must never see a pipe/process handle from a new RPC. */
    h->failure_stage="previous stream stop";
    int drained=xz_receive_stop(h);
    if(drained) return drained;
    int rc=0;
    xz_operation_begin(h,operation,"tune-stop");
    EnterCriticalSection(&h->command_lock);
    if(h->session.request_valid) rc=mb86H_session_stop(&h->session,&session_ops,h,h->session.source);
    LeaveCriticalSection(&h->command_lock);
    if(rc) { xz_operation_end(h); return rc; }

    rc=xz_operation_check(h,operation);
    if(rc) { xz_operation_end(h); return rc; }
    h->failure_stage="RF acquisition";
    xz_log(h,"tune stage=%s system=%u frequency=%u",h->failure_stage,request->system,request->frequency);
    rc=mb86H_session_tune(&h->session,&session_ops,h,request->system,request,false);
    if(!rc) {
        h->failure_stage="USB receive start";
        rc=xz_receive_start(h);
    }
    if(!rc) {
        h->failure_stage="stream activation";
        rc=mb86H_session_stream(&h->session,&session_ops,h,request->system,true);
    }
    /* SERVICE outlives this client RPC.  It must never retain its process or
     * pipe HANDLE (or the caller's stack operation) after the stream starts. */
    xz_operation_end(h);
    if(!rc) {
        h->service_worker=CreateThread(NULL,0,service_thread,h,0,NULL);
        if(!h->service_worker) { h->failure_stage="service worker start"; rc=-EIO; }
    }
    if(rc) xz_receive_stop(h);
    xz_log(h,"tune system=%u frequency=%u tsid=%u stage=%s rc=%d",
        request->system,request->frequency,request->stream_id,rc?h->failure_stage:"complete",rc);
    if(!rc) h->failure_stage=NULL;
    return rc;
}
const char *xz_failure_stage(struct xz_host *h)
{ return h && h->failure_stage ? h->failure_stage : "device preparation"; }
int xz_detach(struct xz_host *h,const struct xz_operation *operation)
{
    if(!h) return -EINVAL;
    /* A lost client cannot veto the device stop.  Its deadline still bounds
     * the request when supplied; a short owner deadline is used otherwise. */
    struct xz_operation owner=operation ? *operation : (struct xz_operation){0};
    if(!owner.deadline) owner.deadline=GetTickCount64()+30000;
    int drained=xz_receive_stop(h);
    if(drained) return drained;
    int rc=0;
    xz_operation_begin(h,&owner,"detach");
    EnterCriticalSection(&h->command_lock);
    if(h->session.request_valid)
        rc=mb86H_session_stop(&h->session,&session_ops,h,h->session.source);
    LeaveCriticalSection(&h->command_lock);
    if(!rc) {
        rc=mb86H_session_wait_stopped(&h->session,&session_ops,h);
        if(!rc) h->session.request_valid=false;
    }
    xz_operation_end(h);
    xz_log(h,"detach stop_rc=%d drain_rc=%d retained_loader=1",rc,drained);
    return rc ? rc : drained;
}
int xz_close(struct xz_host **owner)
{
    if(!owner || !*owner) return 0;
    struct xz_host *h=*owner;
    int rc=xz_receive_stop(h);
    if(rc) return rc; /* Caller must retain the live callback owner. */
    struct mb86H_loader_ops ops=loader_ops(h);
    struct xz_operation restore_operation={0};
    restore_operation.deadline=GetTickCount64()+180000;
    xz_operation_begin(h,&restore_operation,"restore");
    rc=mb86H_loader_restore(&h->loader,&ops,h);
    xz_operation_end(h);
    xz_log(h,"close restore rc=%d transport_win32=%ld",rc,InterlockedCompareExchange(&h->transport_error,0,0));
    if(rc) xz_log(h,"restoration incomplete; snapshot retained; no automatic retry or resident admission");
    destroy(h); *owner=NULL;
    return rc;
}
DWORD xz_wait(struct xz_host *h,DWORD timeout)
{
    if(!h) return WAIT_ABANDONED;
    DWORD result=WaitForSingleObject(h->ready,timeout);
    EnterCriticalSection(&h->queue_lock);
    if(h->receiver.stats.error || !h->receiving) result=WAIT_ABANDONED;
    LeaveCriticalSection(&h->queue_lock);
    return result;
}
DWORD xz_ready(struct xz_host *h)
{
    if(!h) return 0;
    EnterCriticalSection(&h->queue_lock);
    DWORD count=(DWORD)((h->receiver.used+255)/256);
    LeaveCriticalSection(&h->queue_lock);
    return count;
}
int xz_read(struct xz_host *h,BYTE *data,DWORD capacity,DWORD *size,DWORD *remain)
{
    if(!h || !data || !size) return -EINVAL;
    size_t bytes=0;
    EnterCriticalSection(&h->queue_lock);
    int rc=mb86H_ts_receiver_core_read(&h->receiver,
         mb86H_ts_receiver_core_generation(&h->receiver),data,capacity,&bytes);
    *size=(DWORD)bytes;
    if(remain) *remain=(DWORD)((h->receiver.used+255)/256);
    if(!h->receiver.used && !h->receiver.stats.error) ResetEvent(h->ready);
    LeaveCriticalSection(&h->queue_lock);
    return rc;
}
void xz_purge(struct xz_host *h)
{
    if(!h) return;
    EnterCriticalSection(&h->queue_lock);
    mb86H_ts_receiver_core_discard_buffered(&h->receiver);
    h->receiver.discard_mux_frame=h->mux.used!=0;
    if(!h->receiver.stats.error) ResetEvent(h->ready);
    LeaveCriticalSection(&h->queue_lock);
}
