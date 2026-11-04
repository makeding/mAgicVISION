/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "ifd_abi.h"
#include "mb86H/card.h"
#include "mb86H/loader.h"
#include "mb86H/device.h"
#include "mb86H/protocol.h"
#include "mb86H/usb/libusb.h"
#include "firmware_blob.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <syslog.h>
#include <unistd.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    struct usb_device *usb;
    struct mb86H_loader loader;
    struct mb86H_session session;
    struct mb86H_loader_ops ops;
    DWORD lun;
    struct mb86H_card card;
    int opened,transport_failed;
    char snapshot[1024];
} reader;
static uint32_t now(void *unused)
{struct timespec t;(void)unused;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
static void delay(void *unused,unsigned ms)
{struct timespec t={ms/1000,(long)(ms%1000)*1000000};(void)unused;while(nanosleep(&t,&t)&&errno==EINTR){}}
static void *allocate(void *unused,size_t n){(void)unused;return malloc(n);}
static void release(void *unused,void *p){(void)unused;free(p);}
static int transport_result(int rc)
{if(rc){reader.transport_failed=rc;syslog(LOG_ERR,"mb86H IFD terminal transport failure=%d",rc);}return rc;}
static void mismatch(void *unused,uint32_t address,uint32_t expected,uint32_t observed)
{(void)unused;syslog(LOG_ERR,"mb86H IFD restore mismatch address=%08x expected=%08x observed=%08x",address,expected,observed);}
static int read_window(void *unused,uint32_t a,void *p,uint16_t n)
{
    (void)unused;if(reader.transport_failed)return reader.transport_failed;
    return transport_result(usb_libusb_boot0_read(reader.usb,a,p,n));
}
static int write_window(void *unused,uint32_t a,const void *p,uint16_t n)
{
    (void)unused;if(reader.transport_failed)return reader.transport_failed;
    return transport_result(usb_libusb_boot0_write(reader.usb,a,p,n));
}
static int block_writes(void *unused,bool enabled)
{
    (void)unused;
    if(reader.transport_failed)return reader.transport_failed;
    return transport_result(usb_libusb_set_block_writes(reader.usb,enabled));
}
static int exchange(void *unused,const uint8_t tx[64],uint8_t rx[64])
{
    (void)unused;if(reader.transport_failed)return reader.transport_failed;
    const struct mb86H_transport *transport=usb_libusb_transport(reader.usb);
    return transport_result(transport->ops->exchange(transport->opaque,tx,rx));
}
static const struct mb86H_session_ops session_ops={.exchange=exchange,.now_ms=now,.delay_ms=delay};
static int write_all(int fd,const void *p,size_t n)
{const uint8_t *b=p;while(n){ssize_t k=write(fd,b,n);if(k<0&&errno==EINTR)continue;if(k<=0)return -EIO;b+=k;n-=(size_t)k;}return 0;}
static int snapshot_write(void *opaque,const void *data,size_t size)
{return write_all(*(const int *)opaque,data,size);}
static int snapshot(void *unused,const struct mb86H_loader *l)
{
    (void)unused;
    const char *dir=getenv("MB86H_PCSC_STATE_DIR");
    if(!dir) {
#ifdef __APPLE__
        dir="/Library/Application Support/mb86H/pcsc";
#else
        dir="/var/lib/mb86H/pcsc";
#endif
    }
    DIR *directory=opendir(dir);if(!directory)return -errno;
    struct dirent *entry;size_t retained=0;
    while((entry=readdir(directory))) {
        char path[1024];struct stat st;
        if(strncmp(entry->d_name,"recovery-",9))continue;
        int k=snprintf(path,sizeof path,"%s/%s",dir,entry->d_name);
        if(k>0&&(size_t)k<sizeof path&&!lstat(path,&st)&&S_ISREG(st.st_mode))retained+=(size_t)st.st_size;
    }
    closedir(directory);
    size_t payload=mb86H_loader_snapshot_size(l),bytes=payload+8;
    if(!payload)return -EINVAL;
    if(bytes>8u*1024u*1024u || retained>8u*1024u*1024u-bytes)return -ENOSPC;
    int n=snprintf(reader.snapshot,sizeof reader.snapshot,"%s/recovery-%lu-%ld-%u.bin",dir,(unsigned long)geteuid(),(long)getpid(),now(0));
    if(n<0||(size_t)n>=sizeof reader.snapshot)return -EINVAL;
    int fd=open(reader.snapshot,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return -errno;
    /* Keep each existing recovery format paired with its device profile. */
    int rc=write_all(fd,l->target==MB86H_BOOTSTRAP_DT295?"MBBOOT04":l->is_torne?"MBSCREC4":"MBSCREC2",8);
    if(!rc)rc=mb86H_loader_snapshot_write(l,snapshot_write,&fd);
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(rc){unlink(reader.snapshot);reader.snapshot[0]=0;}
    return rc;
}
static RESPONSECODE error(int rc)
{
    if(!rc)return IFD_SUCCESS;
    if(reader.transport_failed)return IFD_COMMUNICATION_ERROR;
    if(rc==-EOPNOTSUPP)return IFD_NOT_SUPPORTED;
    if(rc==-EMSGSIZE)return IFD_ERROR_INSUFFICIENT_BUFFER;
    if(rc==-ETIMEDOUT)return IFD_RESPONSE_TIMEOUT;
    return IFD_COMMUNICATION_ERROR;
}
static int card_command(void *unused,unsigned op,const uint8_t data[40],uint8_t rx[64])
{
    (void)unused;
    return mb86H_session_command(&reader.session,&session_ops,0,
                                    MB86H_RESIDENT_COMMAND_CARD,op,data,rx);
}
static const struct mb86H_card_ops card_ops={card_command,now,delay};
static int valid(DWORD lun){return reader.opened&&reader.lun==lun&&!reader.transport_failed;}
static int target_from_name(const char *name,uint16_t *vendor,uint16_t *product)
{
    static const struct {const char *name;uint16_t vendor,product;} targets[]={
        {PCSC_READER_XZ_NAME,USB_VENDOR_ID,USB_PRODUCT_ID},
        {PCSC_READER_TORNE_NAME,USB_TORNE_VENDOR_ID,USB_TORNE_PRODUCT_ID},
        {PCSC_READER_DT295_NAME,MB86H_PIX_DT295_VENDOR_ID,MB86H_PIX_DT295_PRODUCT_ID},
    };
    if(!name)return -EINVAL;
    for(unsigned i=0;i<sizeof targets/sizeof targets[0];++i) {
        char usb_name[32];
        int n=snprintf(usb_name,sizeof usb_name,"usb:%04x/%04x",targets[i].vendor,targets[i].product);
        if(!strcmp(name,targets[i].name) ||
           (!strncmp(name,usb_name,(size_t)n) && (!name[n] || name[n]==':'))) {
            *vendor=targets[i].vendor;*product=targets[i].product;
            return 0;
        }
    }
    return -EINVAL;
}
RESPONSECODE IFDHCreateChannelByName(DWORD lun,LPSTR name)
{
    int rc=0;unsigned count=0;uint16_t vendor=0,product=0;pthread_mutex_lock(&lock);
    if(reader.opened){rc=-EBUSY;goto done;}
    syslog(LOG_NOTICE,"mb86H IFD CreateChannelByName lun=%u name=%s",(unsigned)lun,name?name:"(null)");
    /* macOS passes the Info.plist friendly name, Linux passes usb:VID/PID. */
    rc=target_from_name(name,&vendor,&product);if(rc)goto done;
    memset(&reader,0,sizeof reader);reader.lun=lun;mb86H_card_init(&reader.card,&card_ops,0);
    rc=usb_libusb_count_target(vendor,product,&count);if(rc||count!=1){rc=-ENODEV;goto done;}
    rc=usb_libusb_open_target(&reader.usb,vendor,product);if(rc)goto done;
    reader.ops=(struct mb86H_loader_ops){.window_read=read_window,.window_write=write_window,
        .session=&reader.session,.session_ops=&session_ops,.now_ms=now,.delay_ms=delay,
        .allocate=allocate,.release=release,.snapshot=snapshot,.block_writes=block_writes,
        .mismatch=mismatch};
    rc=mb86H_device_firmware_parse(mb86H_device_find(vendor,product),&reader.loader,pcsc_firmware,sizeof pcsc_firmware);
    if(!rc)rc=reader.loader.is_torne?mb86H_loader_start(&reader.loader,&reader.ops,0):
                              mb86H_loader_install(&reader.loader,&reader.ops,0);
    if(!rc){reader.opened=1;fprintf(stderr,"mb86H IFD opened release FW %s\n",PCSC_FIRMWARE_SHA256);}
    else {
        int restored=reader.transport_failed?-EIO:mb86H_loader_restore(&reader.loader,&reader.ops,0);
        if(!restored&&reader.snapshot[0])unlink(reader.snapshot);
        fprintf(stderr,"mb86H IFD open failed rc=%d restore=%d snapshot=%s\n",rc,restored,reader.snapshot);
        usb_libusb_close(reader.usb);reader.usb=0;
    }
done:syslog(rc?LOG_ERR:LOG_NOTICE,"mb86H IFD open result=%d",rc);pthread_mutex_unlock(&lock);return rc?IFD_COMMUNICATION_ERROR:IFD_SUCCESS;
}
RESPONSECODE IFDHCreateChannel(DWORD lun,DWORD channel)
{(void)channel;return IFDHCreateChannelByName(lun,PCSC_READER_XZ_NAME);}
RESPONSECODE IFDHCloseChannel(DWORD lun)
{
    int rc=0;pthread_mutex_lock(&lock);
    if(!reader.opened||reader.lun!=lun){pthread_mutex_unlock(&lock);return IFD_NO_SUCH_DEVICE;}
    if(!reader.transport_failed)rc=mb86H_loader_restore(&reader.loader,&reader.ops,0);else rc=-EIO;
    if(!rc&&reader.snapshot[0])unlink(reader.snapshot);
    fprintf(stderr,"mb86H IFD close restore=%d snapshot=%s\n",rc,rc?reader.snapshot:"removed");
    int closed=usb_libusb_close(reader.usb);if(!rc)rc=closed;
    reader.usb=0;reader.opened=0;reader.card.atr_length=0;
    pthread_mutex_unlock(&lock);return error(rc);
}
RESPONSECODE IFDHICCPresence(DWORD lun)
{
    uint8_t state;RESPONSECODE out;
    pthread_mutex_lock(&lock);
    if(!valid(lun))out=IFD_NO_SUCH_DEVICE;
    else {
        int rc=mb86H_card_status(&reader.card,&state);
        out=rc?error(rc):state==MB86H_CARD_ABSENT?IFD_ICC_NOT_PRESENT:IFD_ICC_PRESENT;
    }
    pthread_mutex_unlock(&lock);return out;
}
RESPONSECODE IFDHPowerICC(DWORD lun,DWORD action,PUCHAR atr,PDWORD length)
{
    int rc;size_t n=MB86H_CARD_MAX_ATR;
    if(length)*length=0;
    pthread_mutex_lock(&lock);
    if(!valid(lun)){pthread_mutex_unlock(&lock);return IFD_NO_SUCH_DEVICE;}
    /* PowerICC's ATR buffer is MAX_ATR_SIZE; macOS initializes length to zero. */
    if(!length || ((action==IFD_POWER_UP || action==IFD_RESET)&&!atr)) {
        pthread_mutex_unlock(&lock);return IFD_ERROR_INSUFFICIENT_BUFFER;
    }
    if(action==IFD_POWER_DOWN)rc=mb86H_card_power_off(&reader.card);
    else if(action==IFD_POWER_UP)rc=mb86H_card_power_on(&reader.card,atr,&n);
    else if(action==IFD_RESET)rc=mb86H_card_reset(&reader.card,atr,&n);
    else rc=-EINVAL;
    if(!rc && action!=IFD_POWER_DOWN)*length=(DWORD)n;
    syslog(rc?LOG_ERR:LOG_NOTICE,"mb86H IFD PowerICC action=%u result=%d firmware=%d atr_length=%u",
           (unsigned)action,rc,reader.card.firmware_result,(unsigned)*length);
    pthread_mutex_unlock(&lock);return error(rc);
}
RESPONSECODE IFDHTransmitToICC(DWORD lun,SCARD_IO_HEADER send_pci,PUCHAR tx,DWORD tx_length,PUCHAR rx,PDWORD rx_length,PSCARD_IO_HEADER recv_pci)
{
    uint8_t response[MB86H_CARD_MAX_RX];size_t n=sizeof response;
    DWORD capacity=rx_length?*rx_length:0;int rc;
    if(rx_length)*rx_length=0;
    pthread_mutex_lock(&lock);
    if(!valid(lun)){pthread_mutex_unlock(&lock);return IFD_NO_SUCH_DEVICE;}
    if(reader.card.parameters.protocol>1 || send_pci.Protocol!=reader.card.parameters.protocol) {
        pthread_mutex_unlock(&lock);return IFD_PROTOCOL_NOT_SUPPORTED;
    }
    if(!rx || !rx_length || !capacity){pthread_mutex_unlock(&lock);return IFD_ERROR_INSUFFICIENT_BUFFER;}
    rc=mb86H_card_transfer(&reader.card,tx,tx_length,response,&n);
    if(!rc) {
        *rx_length=(DWORD)n;
        if(n>capacity)rc=-EMSGSIZE;
        else {memcpy(rx,response,n);if(recv_pci){recv_pci->Protocol=send_pci.Protocol;recv_pci->Length=sizeof *recv_pci;}}
    }
    pthread_mutex_unlock(&lock);return error(rc);
}
RESPONSECODE IFDHGetCapabilities(DWORD lun,DWORD tag,PDWORD length,PUCHAR value)
{
    RESPONSECODE rc=IFD_SUCCESS;pthread_mutex_lock(&lock);
    if(!valid(lun))rc=IFD_NO_SUCH_DEVICE;
    else if(!length||!value)rc=IFD_ERROR_TAG;
    else if(tag==TAG_IFD_ATR||tag==0x90303) {
        if(*length<reader.card.atr_length)rc=IFD_ERROR_INSUFFICIENT_BUFFER;
        else {memcpy(value,reader.card.atr,reader.card.atr_length);*length=reader.card.atr_length;}
    } else if(tag==TAG_IFD_SLOTS_NUMBER||tag==TAG_IFD_SIMULTANEOUS_ACCESS||tag==TAG_IFD_THREAD_SAFE||tag==TAG_IFD_SLOT_THREAD_SAFE) {
        if(*length<1)rc=IFD_ERROR_INSUFFICIENT_BUFFER;else {value[0]=1;*length=1;}
    } else rc=IFD_ERROR_TAG;
    pthread_mutex_unlock(&lock);return rc;
}
RESPONSECODE IFDHSetCapabilities(DWORD lun,DWORD tag,DWORD n,PUCHAR p)
{(void)lun;(void)tag;(void)n;(void)p;return IFD_ERROR_VALUE_READ_ONLY;}
RESPONSECODE IFDHSetProtocolParameters(DWORD lun,DWORD protocol,UCHAR flags,UCHAR a,UCHAR b,UCHAR c)
{
    (void)a;(void)b;(void)c;pthread_mutex_lock(&lock);
    RESPONSECODE rc=IFD_NO_SUCH_DEVICE;
    if(valid(lun)&&reader.card.atr_length) {
        DWORD selected=reader.card.parameters.protocol==0?SCARD_PROTOCOL_T0:reader.card.parameters.protocol==1?SCARD_PROTOCOL_T1:0;
        rc=selected && protocol==selected && !flags?IFD_SUCCESS:IFD_PROTOCOL_NOT_SUPPORTED;
    }
    pthread_mutex_unlock(&lock);return rc;
}
RESPONSECODE IFDHControl(DWORD lun,DWORD code,PUCHAR tx,DWORD n,PUCHAR rx,DWORD cap,LPDWORD got)
{(void)lun;(void)code;(void)tx;(void)n;(void)rx;(void)cap;if(got)*got=0;return IFD_NOT_SUPPORTED;}
