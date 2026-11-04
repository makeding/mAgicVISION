#include "transport.h"
#include <setupapi.h>
#include <objbase.h>
#include <usbiodef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "mb86H/device.h"
#include "mb86H/usb/window.h"

void xz_log(struct xz_host *h, const char *format, ...)
{
    char text[1024]; va_list ap;
    va_start(ap, format); vsnprintf(text, sizeof(text), format, ap); va_end(ap);
    OutputDebugStringA(text);
    EnterCriticalSection(&h->log_lock);
    if (h->log) {
        SYSTEMTIME t; GetSystemTime(&t);
        fprintf(h->log, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s\n",
                t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,text);
        fflush(h->log);
    }
    LeaveCriticalSection(&h->log_lock);
}
static int usb_error(struct xz_host *h, const char *where, DWORD error)
{
    InterlockedCompareExchange(&h->transport_error, (LONG)(error ? error : ERROR_GEN_FAILURE), 0);
    xz_log(h, "%s win32=%lu (0x%08lx); device commands fenced",where,error,error);
    return -EIO;
}
int xz_operation_check(struct xz_host *h,const struct xz_operation *operation)
{
    if(!operation) return 0;
    if(operation->deadline && GetTickCount64()>=operation->deadline) {
        xz_log(h,"operation stop cause=deadline stage=%s",h->progress_stage?h->progress_stage:"none");
        return -ETIMEDOUT;
    }
    if(operation->peer && operation->peer!=INVALID_HANDLE_VALUE &&
       WaitForSingleObject(operation->peer,0)==WAIT_OBJECT_0) {
        xz_log(h,"operation stop cause=client-process-exit stage=%s",h->progress_stage?h->progress_stage:"none");
        return -ECANCELED;
    }
    if(operation->pipe && operation->pipe!=INVALID_HANDLE_VALUE) {
        DWORD available=0;
        if(!PeekNamedPipe(operation->pipe,NULL,0,NULL,&available,NULL)) {
            DWORD error=GetLastError();
            if(error==ERROR_BROKEN_PIPE || error==ERROR_PIPE_NOT_CONNECTED ||
               error==ERROR_NO_DATA) {
                xz_log(h,"operation stop cause=control-pipe-disconnect win32=%lu",error);
                return -ECANCELED;
            }
        }
    }
    return 0;
}
void xz_operation_begin(struct xz_host *h,const struct xz_operation *operation,
                        const char *stage)
{
    h->operation=operation;
    h->progress_stage=stage;
    h->operation_started=h->progress_updated=GetTickCount64();
    h->progress_bytes=0;
    h->progress_controls=0;
    h->progress_next=64*1024;
    h->progress_next_controls=1024;
    xz_log(h,"operation begin stage=%s deadline=%llu peer=%s",stage,
           operation ? operation->deadline : 0,
           operation && operation->peer ? "set" : "none");
}
void xz_operation_end(struct xz_host *h)
{
    if(h->progress_stage)
        xz_log(h,"operation end stage=%s bytes=%llu controls=%llu",h->progress_stage,
               h->progress_bytes,h->progress_controls);
    h->operation=NULL;
    h->progress_stage=NULL;
}
static void progress_log(struct xz_host *h,const char *kind)
{
    xz_log(h,"operation progress stage=%s kind=%s bytes=%llu controls=%llu",
           h->progress_stage ? h->progress_stage : "transport",kind,
           h->progress_bytes,h->progress_controls);
}
void xz_progress_window(struct xz_host *h,const char *kind,size_t bytes)
{
    h->progress_bytes+=bytes;
    if(h->progress_bytes<h->progress_next) return;
    progress_log(h,kind);
    h->progress_next=h->progress_bytes+64*1024;
}
void xz_progress_control(struct xz_host *h,unsigned bytes)
{
    (void)bytes;
    ++h->progress_controls;
    if(h->progress_controls<h->progress_next_controls) return;
    progress_log(h,"control");
    h->progress_next_controls=h->progress_controls+1024;
}
static wchar_t *interface_path(HDEVINFO list,SP_DEVINFO_DATA *device)
{
    HKEY key=SetupDiOpenDevRegKey(list,device,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_READ);
    if(key==INVALID_HANDLE_VALUE) return NULL;
    wchar_t guids[4096]={0}; DWORD type=0,bytes=sizeof(guids)-2*sizeof(wchar_t);
    LONG status=RegQueryValueExW(key,L"DeviceInterfaceGUIDs",NULL,&type,(BYTE *)guids,&bytes);
    if(status==ERROR_FILE_NOT_FOUND) {
        bytes=sizeof(guids)-2*sizeof(wchar_t);
        status=RegQueryValueExW(key,L"DeviceInterfaceGUID",NULL,&type,(BYTE *)guids,&bytes);
    }
    RegCloseKey(key);
    if(status!=ERROR_SUCCESS || (type!=REG_SZ && type!=REG_MULTI_SZ)) return NULL;
    wchar_t target_id[4096];
    if(!SetupDiGetDeviceInstanceIdW(list,device,target_id,4096,NULL)) return NULL;
    for(wchar_t *text=guids;*text;text+=wcslen(text)+1) {
        GUID guid;
        if(FAILED(CLSIDFromString(text,&guid))) continue;
        HDEVINFO interfaces=SetupDiGetClassDevsW(&guid,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        if(interfaces==INVALID_HANDLE_VALUE) continue;
        wchar_t *path=NULL;
        for(DWORD i=0;;++i) {
            SP_DEVICE_INTERFACE_DATA iface={.cbSize=sizeof(iface)};
            SP_DEVINFO_DATA found={.cbSize=sizeof(found)};
            if(!SetupDiEnumDeviceInterfaces(interfaces,NULL,&guid,i,&iface)) break;
            DWORD size=0;
            SetupDiGetDeviceInterfaceDetailW(interfaces,&iface,NULL,0,&size,NULL);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail=calloc(1,size);
            if(!detail) break;
            detail->cbSize=sizeof(*detail);
            wchar_t found_id[4096];
            if(SetupDiGetDeviceInterfaceDetailW(interfaces,&iface,detail,size,NULL,&found) &&
               SetupDiGetDeviceInstanceIdW(interfaces,&found,found_id,4096,NULL) &&
               !_wcsicmp(found_id,target_id)) path=_wcsdup(detail->DevicePath);
            free(detail);
            if(path) break;
        }
        SetupDiDestroyDeviceInfoList(interfaces);
        if(path) return path;
    }
    return NULL;
}
int xz_transport_open(struct xz_host *h)
{
    HDEVINFO list=SetupDiGetClassDevsW(NULL,L"USB",NULL,DIGCF_PRESENT|DIGCF_ALLCLASSES);
    wchar_t *path=NULL;
    unsigned matches=0;
    if(list==INVALID_HANDLE_VALUE) return usb_error(h,"SetupDiGetClassDevs",GetLastError());
    for(DWORD i=0;;++i) {
        SP_DEVINFO_DATA dev={.cbSize=sizeof(dev)};
        wchar_t ids[4096]={0};
        if(!SetupDiEnumDeviceInfo(list,i,&dev)) {
            DWORD error=GetLastError();
            if(error!=ERROR_NO_MORE_ITEMS) { SetupDiDestroyDeviceInfoList(list); free(path); return usb_error(h,"SetupDiEnumDeviceInfo",error); }
            break;
        }
        if(!SetupDiGetDeviceRegistryPropertyW(list,&dev,SPDRP_HARDWAREID,NULL,
                      (BYTE *)ids,sizeof(ids)-2*sizeof(wchar_t),NULL)) continue;
        BOOL target=FALSE;
        for(wchar_t *id=ids;*id;id+=wcslen(id)+1)
            if(!_wcsicmp(id,L"USB\\VID_04BB&PID_0530") ||
               !_wcsnicmp(id,L"USB\\VID_04BB&PID_0530&REV_",26)) target=TRUE;
        if(!target) continue;
        ++matches;
        if(!path) path=interface_path(list,&dev);
    }
    SetupDiDestroyDeviceInfoList(list);
    if(matches!=1 || !path) {
        xz_log(h,"expected one 04bb:0530 WinUSB interface, found=%u path=%s; bind XZ to WinUSB",matches,path?"registered":"missing");
        free(path); return matches>1 ? -EBUSY : -ENODEV;
    }
    xz_log(h,"selected path=%ls",path);
    h->file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
    free(path);
    if(h->file==INVALID_HANDLE_VALUE) return usb_error(h,"CreateFile",GetLastError());
    if(!WinUsb_Initialize(h->file,&h->usb)) return usb_error(h,"WinUsb_Initialize (install WinUSB)",GetLastError());
    USB_DEVICE_DESCRIPTOR descriptor; ULONG actual=0;
    if(!WinUsb_GetDescriptor(h->usb,USB_DEVICE_DESCRIPTOR_TYPE,0,0,
                              (BYTE *)&descriptor,sizeof(descriptor),&actual))
        return usb_error(h,"WinUsb_GetDescriptor",GetLastError());
    if(actual!=sizeof(descriptor) || !mb86H_gv_mvp_xz_matches(descriptor.idVendor,descriptor.idProduct)) return -ENODEV;
    USB_INTERFACE_DESCRIPTOR iface;
    if(!WinUsb_QueryInterfaceSettings(h->usb,0,&iface)) return usb_error(h,"WinUsb_QueryInterfaceSettings",GetLastError());
    if(iface.bInterfaceNumber!=0) return -ENODEV;
    BOOL found=FALSE;
    for(UCHAR i=0;i<iface.bNumEndpoints;++i) {
        WINUSB_PIPE_INFORMATION pipe;
        if(!WinUsb_QueryPipe(h->usb,0,i,&pipe)) return usb_error(h,"WinUsb_QueryPipe",GetLastError());
        if(pipe.PipeId==USB_STREAM_ENDPOINT && pipe.PipeType==UsbdPipeTypeBulk) found=TRUE;
    }
    if(!found) return -ENODEV;
    ULONG timeout=20000;
    if(!WinUsb_SetPipePolicy(h->usb,0,PIPE_TRANSFER_TIMEOUT,sizeof(timeout),&timeout)) return usb_error(h,"control timeout policy",GetLastError());
    xz_log(h,"opened 04bb:0530 bcdDevice=%04x interface=0 EP%02x bulk-IN",descriptor.bcdDevice,USB_STREAM_ENDPOINT);
    return 0;
}
void xz_transport_close(struct xz_host *h)
{
    if(h->usb) { WinUsb_Free(h->usb); h->usb=NULL; }
    if(h->file && h->file!=INVALID_HANDLE_VALUE) CloseHandle(h->file);
    h->file=INVALID_HANDLE_VALUE;
}
static DWORD control_timeout_ms(const struct xz_operation *operation,unsigned timeout)
{
    ULONGLONG limit=timeout ? timeout : 20000;
    if(operation && operation->deadline) {
        ULONGLONG now=GetTickCount64();
        if(now>=operation->deadline) return 0;
        ULONGLONG remaining=operation->deadline-now;
        if(remaining<limit) limit=remaining;
    }
    return limit>MAXDWORD ? MAXDWORD : (DWORD)limit;
}
int xz_control(struct xz_host *h,BYTE direction,WORD value,WORD index,void *data,
               WORD size,unsigned timeout)
{
    WINUSB_SETUP_PACKET setup={direction,USB_WINDOW_VENDOR_REQUEST,value,index,size};
    ULONG actual=0;
    OVERLAPPED pending={0};
    HANDLE event=NULL;
    BOOL done;
    DWORD error=ERROR_SUCCESS;
    DWORD transfer_timeout;
    ULONGLONG started;
    if(InterlockedCompareExchange(&h->transport_error,0,0)) return -EIO;
    int state=xz_operation_check(h,h->operation);
    if(state) return state;
    event=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!event) return usb_error(h,"CreateEvent control",GetLastError());
    pending.hEvent=event;
    done=WinUsb_ControlTransfer(h->usb,setup,data,size,&actual,&pending);
    if(!done && (error=GetLastError())==ERROR_IO_PENDING) {
        transfer_timeout=control_timeout_ms(h->operation,timeout);
        started=GetTickCount64();
        for(;;) {
            ULONGLONG elapsed=GetTickCount64()-started;
            DWORD remaining=elapsed>=transfer_timeout ? 0 : transfer_timeout-(DWORD)elapsed;
            DWORD wait=WaitForSingleObject(event,remaining>50 ? 50 : remaining);
            if(wait==WAIT_OBJECT_0) break;
            state=xz_operation_check(h,h->operation);
            if(wait==WAIT_FAILED && !state) {
                error=GetLastError();
                state=-EIO;
            }
            if(!state && GetTickCount64()-started>=transfer_timeout) state=-ETIMEDOUT;
            if(!state) continue;
            /* This transfer may have reached the device.  Fence it and drain
             * the exact OVERLAPPED before its storage may go away; never replay. */
            if(!CancelIoEx(h->file,&pending) && GetLastError()!=ERROR_NOT_FOUND)
                error=GetLastError();
            done=WinUsb_GetOverlappedResult(h->usb,&pending,&actual,TRUE);
            DWORD drained=done ? ERROR_SUCCESS : GetLastError();
            DWORD reason=state==-ETIMEDOUT ? ERROR_TIMEOUT :
                state==-ECANCELED ? ERROR_OPERATION_ABORTED :
                error ? error : ERROR_OPERATION_ABORTED;
            CloseHandle(event);
            xz_log(h,"EP0 cancelled type=%02x value=%04x index=%04x length=%u state=%d reason_win32=%lu drained_win32=%lu",
                   direction,value,index,size,state,reason,drained);
            usb_error(h,state==-ETIMEDOUT ? "WinUsb_ControlTransfer timeout" :
                      "WinUsb_ControlTransfer cancelled",reason);
            return state;
        }
        done=WinUsb_GetOverlappedResult(h->usb,&pending,&actual,FALSE);
        error=done ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(event);
    if(!done) {
        xz_log(h,"EP0 type=%02x request=%02x value=%04x index=%04x length=%u actual=%lu",
               direction,USB_WINDOW_VENDOR_REQUEST,value,index,size,actual);
        return usb_error(h,"WinUsb_ControlTransfer",error);
    }
    xz_progress_control(h,size);
    if(actual!=size) return usb_error(h,"short EP0 transfer",ERROR_BAD_LENGTH);
    return 0;
}
static DWORD WINAPI receive_thread(void *opaque)
{
    struct xz_host *h=opaque;
    BYTE buffer[16384];
    OVERLAPPED pending={0};
    pending.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!pending.hEvent) {
        EnterCriticalSection(&h->queue_lock);
        mb86H_ts_receiver_core_receive(&h->receiver,-EIO,NULL,0);
        SetEvent(h->ready); LeaveCriticalSection(&h->queue_lock);
        return 1;
    }
    while(WaitForSingleObject(h->stop,0)==WAIT_TIMEOUT) {
        ULONG bytes=0; ResetEvent(pending.hEvent);
        BOOL done=WinUsb_ReadPipe(h->usb,USB_STREAM_ENDPOINT,buffer,sizeof(buffer),&bytes,&pending);
        DWORD error=done?ERROR_SUCCESS:GetLastError();
        if(!done && error==ERROR_IO_PENDING) {
            HANDLE events[2]={h->stop,pending.hEvent};
            DWORD wait=WaitForMultipleObjects(2,events,FALSE,INFINITE);
            if(wait!=WAIT_OBJECT_0+1) {
                /* OVERLAPPED and buffer stay live until the completion drains. */
                if(!CancelIoEx(h->file,&pending) && GetLastError()!=ERROR_NOT_FOUND)
                    WinUsb_AbortPipe(h->usb,USB_STREAM_ENDPOINT);
            }
            done=WinUsb_GetOverlappedResult(h->usb,&pending,&bytes,TRUE);
            error=done?ERROR_SUCCESS:GetLastError();
        }
        if(WaitForSingleObject(h->stop,0)==WAIT_OBJECT_0) break;
        if(!done) usb_error(h,"WinUsb_ReadPipe",error);
        EnterCriticalSection(&h->queue_lock);
        int rc=done?mb86H_ts_receiver_mux_receive(&h->mux,&h->receiver,buffer,bytes):-EIO;
        if(rc) mb86H_ts_receiver_core_receive(&h->receiver,rc,NULL,0);
        if(h->receiver.used || h->receiver.stats.error) SetEvent(h->ready);
        LeaveCriticalSection(&h->queue_lock);
        if(!done) break;
        if(rc) { xz_log(h,"receive queue/framing failed rc=%d",rc); break; }
    }
    CloseHandle(pending.hEvent);
    return 0;
}
int xz_receive_start(struct xz_host *h)
{
    if(h->worker || h->service_worker || InterlockedCompareExchange(&h->transport_error,0,0)) return -EBUSY;
    if(!WinUsb_FlushPipe(h->usb,USB_STREAM_ENDPOINT))
        return usb_error(h,"flush retired EP81 input",GetLastError());
    EnterCriticalSection(&h->queue_lock);
    int rc=mb86H_ts_receiver_core_start(&h->receiver,MB86H_TS_PACKET_SIZE);
    mb86H_ts_mux_reset(&h->mux); ResetEvent(h->stop); ResetEvent(h->ready);
    LeaveCriticalSection(&h->queue_lock);
    if(rc) return rc;
    h->worker=CreateThread(NULL,0,receive_thread,h,0,NULL);
    if(!h->worker) return usb_error(h,"CreateThread",GetLastError());
    h->receiving=1;
    return 0;
}
int xz_receive_stop(struct xz_host *h)
{
    SetEvent(h->stop);
    if(h->service_worker) {
        if(WaitForSingleObject(h->service_worker,INFINITE)!=WAIT_OBJECT_0)
            return usb_error(h,"drain SERVICE thread",GetLastError());
        CloseHandle(h->service_worker); h->service_worker=NULL;
    }
    if(h->worker) {
        DWORD result=WaitForSingleObject(h->worker,INFINITE);
        if(result!=WAIT_OBJECT_0) return usb_error(h,"drain receive thread",GetLastError());
        CloseHandle(h->worker); h->worker=NULL;
    }
    EnterCriticalSection(&h->queue_lock);
    mb86H_ts_receiver_core_stop_begin(&h->receiver);
    mb86H_ts_receiver_core_stop_finish(&h->receiver,0);
    SetEvent(h->ready); h->receiving=0;
    LeaveCriticalSection(&h->queue_lock);
    return 0;
}
