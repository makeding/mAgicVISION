#include <assert.h>
#include <stdio.h>
#include "mb86H/card.h"
static int protocol(const uint8_t *a,size_t n)
{struct mb86H_card_parameters p;return mb86H_card_parse_atr(a,n,&p)?-1:p.protocol;}
int main(void)
{
    const uint8_t fixtures[][13]={
        {0x3b,0}, {0x3f,2,0x12,0x34}, {0x3b,0x10,0x95},
        {0x3b,0x80,1,0x81}, {0x3b,0x90,0x95,0x10,0},
        {0x3b,0x80,0x81,0x41,1,0x41},
        {0x3b,0xf0,0x12,0,0xff,0x91,0x81,0xb1,0x7c,0x45,0x1f,3,0x99},
        {0x3b,0x80,2,0x82}
    };
    const unsigned lengths[]={2,4,3,4,5,6,13,4};
    const int protocols[]={0,0,0,1,0,1,1,2};
    for(unsigned i=0;i<sizeof lengths/sizeof lengths[0];++i) {
        assert(protocol(fixtures[i],lengths[i])==protocols[i]);
        for(unsigned n=0;n<lengths[i];++n)assert(protocol(fixtures[i],n)<0);
    }
    const uint8_t bad[]={0x3b,0x80,1,0x80};assert(protocol(bad,sizeof bad)<0);
    puts("PC/SC ATR: T=0/T=1, specific/negotiable, inverse, CRC, identity independence and malformed input passed");
}
