#include "mb86H/ts_mux.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
struct received { unsigned size[2];uint8_t bytes[2][1200]; };
static int sink(void *opaque,unsigned channel,const uint8_t *p,size_t n)
{
    struct received *r=opaque;
    CHECK(r->size[channel]+n<=1200);
    memcpy(r->bytes[channel]+r->size[channel],p,n);
    r->size[channel]+=(unsigned)n;
    return 0;
}
int main(void)
{
    uint8_t source[2][1000],wire[6*512];
    unsigned c,i,offset=0,n=0;
    for(c=0;c<2;++c)for(i=0;i<1000;++i)source[c][i]=(uint8_t)(i*13+c*79);
    for(i=0;i<3;++i)for(c=0;c<2;++c) {
        unsigned bytes=i==2?8:496;
        CHECK(!mb86H_ts_mux_pack(wire+n*512,c,i,source[c]+i*496,bytes));++n;
    }
    for(unsigned chunk=1;chunk<=1025;chunk+=17) {
        struct received r={0};struct mb86H_ts_mux mux;
        mb86H_ts_mux_reset(&mux);
        for(offset=0;offset<sizeof(wire);) {
            unsigned bytes=sizeof(wire)-offset;if(bytes>chunk)bytes=chunk;
            CHECK(!mb86H_ts_mux_feed(&mux,wire+offset,bytes,sink,&r));offset+=bytes;
        }
        CHECK(!mb86H_ts_mux_finish(&mux));
        for(c=0;c<2;++c)CHECK(r.size[c]==1000 && !memcmp(r.bytes[c],source[c],1000));
    }
    {
        struct received r={0};struct mb86H_ts_mux mux;
        mb86H_ts_mux_reset(&mux);
        CHECK(!mb86H_ts_mux_feed(&mux,wire,512,sink,&r));
        CHECK(mb86H_ts_mux_feed(&mux,wire,512,sink,&r)==MB86H_MUX_SEQUENCE);
        mb86H_ts_mux_reset(&mux);
        CHECK(!mb86H_ts_mux_feed(&mux,wire,511,sink,&r));
        CHECK(mb86H_ts_mux_finish(&mux)==MB86H_MUX_INVALID);
        wire[4]=1;mb86H_ts_mux_reset(&mux);
        CHECK(mb86H_ts_mux_feed(&mux,wire,512,sink,&r)==MB86H_MUX_INVALID);
    }
    {
        uint8_t frame[512], payload[8]={1,2,3,4,5,6,7,8};
        struct received r={0};struct mb86H_ts_mux mux;
        CHECK(!mb86H_ts_mux_pack(frame,0,0,payload,sizeof(payload)));
        frame[MB86H_MUX_HEADER+sizeof(payload)]=0xa5;
        mb86H_ts_mux_reset(&mux);
        CHECK(!mb86H_ts_mux_feed(&mux,frame,sizeof(frame),sink,&r));
        CHECK(!mb86H_ts_mux_finish(&mux));
        CHECK(r.size[0]==sizeof(payload) && !memcmp(r.bytes[0],payload,sizeof(payload)));
    }
    return 0;
}
