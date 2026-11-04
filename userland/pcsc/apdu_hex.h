#ifndef MB86H_PCSC_APDU_HEX_H
#define MB86H_PCSC_APDU_HEX_H
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
static int apdu_hex(const char *text,unsigned char *out,size_t capacity)
{
    size_t n=strlen(text);
    if(!n || n%2 || n/2>capacity)return -1;
    for(size_t i=0;i<n/2;++i) {
        char byte[3]={text[2*i],text[2*i+1],0};
        if(!isxdigit((unsigned char)byte[0]) || !isxdigit((unsigned char)byte[1]))return -1;
        out[i]=(unsigned char)strtoul(byte,0,16);
    }
    return (int)(n/2);
}
#endif
