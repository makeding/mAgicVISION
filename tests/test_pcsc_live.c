/* Explicit live IFD test. Never added to unattended CTest. */
#include "../userland/pcsc/ifd_abi.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "mb86H/card.h"
#include "../userland/pcsc/apdu_hex.h"
#define LOAD(name) __typeof__(&name) name##_fn=(__typeof__(&name))dlsym(lib,#name);if(!name##_fn){puts(dlerror());return 1;}
int main(int argc,char **argv)
{
    uint8_t tx[256];int tx_size=0;
    if((argc!=2 && argc!=3) || (argc==3 && (tx_size=apdu_hex(argv[2],tx,sizeof tx))<0))return 2;
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){puts(dlerror());return 1;}
    LOAD(IFDHCreateChannelByName);LOAD(IFDHCloseChannel);LOAD(IFDHICCPresence);LOAD(IFDHPowerICC);LOAD(IFDHSetProtocolParameters);LOAD(IFDHTransmitToICC);
    DWORD lun=0;int result=1;const char *reader=getenv("MB86H_PCSC_TEST_READER");
    if(!reader)reader="I-O DATA GV-MVP/XZ3 Smart Card Reader";
    RESPONSECODE rc=IFDHCreateChannelByName_fn(lun,(LPSTR)reader);printf("create=%ld\n",(long)rc);if(rc)return 1;
    rc=IFDHICCPresence_fn(lun);printf("presence=%ld\n",(long)rc);if(rc!=IFD_ICC_PRESENT)goto done;
    uint8_t atr[64],rx[512];DWORD atr_size=0;
    rc=IFDHPowerICC_fn(lun,IFD_POWER_UP,atr,&atr_size);printf("power=%ld ATR=",(long)rc);for(unsigned i=0;i<atr_size;++i)printf("%02x",atr[i]);puts("");if(rc)goto done;
    struct mb86H_card_parameters params;
    int protocol=mb86H_card_parse_atr(atr,atr_size,&params)?-1:params.protocol;
    if(protocol<0 || protocol>1)goto done;
    rc=IFDHSetProtocolParameters_fn(lun,protocol?SCARD_PROTOCOL_T1:SCARD_PROTOCOL_T0,0,0,0,0);
    printf("protocol T=%d result=%ld\n",protocol,(long)rc);if(rc)goto done;
    if(tx_size) {
        DWORD n=sizeof rx;SCARD_IO_HEADER send={(DWORD)protocol,sizeof send},recv={0};
        rc=IFDHTransmitToICC_fn(lun,send,tx,(DWORD)tx_size,rx,&n,&recv);
        printf("transmit result=%ld bytes=%u",(long)rc,(unsigned)n);
        if(!rc && n>=2)printf(" sw=%02x%02x",rx[n-2],rx[n-1]);
        puts("");if(rc)goto done;
    }
    atr_size=0;rc=IFDHPowerICC_fn(lun,IFD_RESET,atr,&atr_size);printf("warm_reset=%ld atr_bytes=%u\n",(long)rc,(unsigned)atr_size);if(rc||!atr_size)goto done;
    atr_size=sizeof atr;rc=IFDHPowerICC_fn(lun,IFD_POWER_DOWN,atr,&atr_size);printf("power_down=%ld\n",(long)rc);if(rc)goto done;
    result=0;
done:
    rc=IFDHCloseChannel_fn(lun);printf("close=%ld\n",(long)rc);if(rc)result=1;
    dlclose(lib);return result;
}
