/* SPDX-License-Identifier: MIT */
#include "mb86H/card.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
/* Host metadata only. FW owns the negotiated transport and serial timing. */
int mb86H_card_parse_atr(const uint8_t *b,size_t n,struct mb86H_card_parameters *p)
{
    unsigned at=2,group=1,previous=0,protocol=0,first=1,tck=0,y,k;
    uint8_t check=0;
    if(!b || !p || n<2 || n>MB86H_CARD_MAX_ATR || (b[0]!=0x3b && b[0]!=0x3f))return -EPROTO;
    *p=(struct mb86H_card_parameters){.inverse=b[0]==0x3f,.fi_di=0x11,.wi=10,.ifsc=32,.bwi=4,.cwi=13};
    y=b[1]>>4;k=b[1]&15;
    while(y) {
        unsigned map=y;y=0;
        for(unsigned i=0;i<4;++i) {
            unsigned v;
            if(!(map&(1u<<i)))continue;
            if(at>=n)return -EPROTO;
            v=b[at++];
            if(i==0) {
                if(group==1)p->fi_di=(uint8_t)v;
                else if(group==2){p->specific=1;p->protocol=v&15;if(v&0x10)p->fi_di=0x11;}
                else if(previous==1)p->ifsc=(uint8_t)v;
            } else if(i==1) {
                if(group>=3 && previous==1){p->bwi=v>>4;p->cwi=v&15;}
            } else if(i==2) {
                if(group==1)p->guard=(uint8_t)v;
                else if(group==2 && previous==0)p->wi=(uint8_t)v;
                else if(group>=3 && previous==1)p->crc=v&1;
            } else {
                previous=v&15;
                if(first && previous!=15){protocol=previous;first=0;}
                if(previous)tck=1;
                y=v>>4;
            }
        }
        ++group;
    }
    if(at+k+tck!=n)return -EPROTO;
    if(tck){for(unsigned i=1;i<n;++i)check^=b[i];if(check)return -EPROTO;}
    if(!p->specific){p->protocol=(uint8_t)protocol;p->fi_di=0x11;}
    if(p->protocol==1 && (!p->ifsc || p->ifsc==255 || p->bwi>9 || p->cwi>13))return -EPROTO;
    return 0;
}
