/* SPDX-License-Identifier: MIT */
/* Real PC/SC service acceptance, separate from direct IFD dlopen testing. */
#ifdef __APPLE__
#include <PCSC/wintypes.h>
#include <PCSC/winscard.h>
#else
#include <winscard.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "apdu_hex.h"
int main(int argc,char **argv)
{
    unsigned char tx[256];int tx_size=0,at=1;
    int list_only=argc==2 && !strcmp(argv[1],"--list");
    if(list_only)at=2;
    const char *expected_reader="I-O DATA GV-MVP/XZ3 Smart Card Reader";
    if(argc>=3 && !strcmp(argv[1],"--reader")){expected_reader=argv[2];at=3;}
    if(argc>at+1 || (argc==at+1 && (tx_size=apdu_hex(argv[at],tx,sizeof tx))<0)) {
        fprintf(stderr,"usage: %s [--list | --reader NAME] [APDU_HEX] (default: power and ATR only)\n",argv[0]);return 2;
    }
    SCARDCONTEXT ctx;SCARDHANDLE card=0;DWORD size=0,protocol=0;int result=1;
    LONG rc=SCardEstablishContext(SCARD_SCOPE_SYSTEM,NULL,NULL,&ctx);
    printf("SCardEstablishContext=%08x\n",(unsigned)rc);if(rc)return 1;
    rc=SCardListReaders(ctx,NULL,NULL,&size);printf("SCardListReaders=%08x bytes=%u\n",(unsigned)rc,(unsigned)size);
    char *readers=calloc(1,size?size:1),*target=NULL;
    if(rc||!readers)goto done;
    rc=SCardListReaders(ctx,NULL,readers,&size);if(rc)goto done;
    for(char *p=readers;*p;p+=strlen(p)+1){printf("reader=%s\n",p);if(!list_only && strstr(p,expected_reader)){if(target){puts("multiple matching readers; refusing ambiguous selection");goto done;}target=p;}}
    if(list_only){result=0;goto done;}
    if(!target){printf("reader is not registered with the PC/SC service: %s\n",expected_reader);goto done;}
    rc=SCardConnect(ctx,target,SCARD_SHARE_SHARED,SCARD_PROTOCOL_T0|SCARD_PROTOCOL_T1,&card,&protocol);
    printf("SCardConnect=%08x protocol=%u\n",(unsigned)rc,(unsigned)protocol);if(rc)goto done;
    unsigned char atr[64],rx[512];DWORD atr_size=sizeof atr,state=0,name_size=512;char name[512];
    rc=SCardStatus(card,name,&name_size,&state,&protocol,atr,&atr_size);printf("SCardStatus=%08x ATR=",(unsigned)rc);if(!rc)for(unsigned i=0;i<atr_size;++i)printf("%02x",atr[i]);puts("");if(rc)goto done;
    if(!atr_size)goto done;
    if(tx_size) {
        const SCARD_IO_REQUEST *pci=protocol==SCARD_PROTOCOL_T0?SCARD_PCI_T0:SCARD_PCI_T1;
        DWORD n=sizeof rx;rc=SCardTransmit(card,pci,tx,(DWORD)tx_size,NULL,rx,&n);
        printf("SCardTransmit=%08x bytes=%u",(unsigned)rc,(unsigned)n);
        if(!rc && n>=2)printf(" sw=%02x%02x",rx[n-2],rx[n-1]);
        puts("");if(rc)goto done;
    }
    result=0;
done:
    if(card){rc=SCardDisconnect(card,SCARD_UNPOWER_CARD);printf("SCardDisconnect=%08x\n",(unsigned)rc);if(rc)result=1;}
    free(readers);SCardReleaseContext(ctx);return result;
}
