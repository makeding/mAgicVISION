/* SPDX-License-Identifier: MIT */
#include "mb86H/device_query.h"
#include "mb86H/protocol.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static int query(struct mb86H_session *session,const struct mb86H_session_ops *ops,
    void *opaque,unsigned page,unsigned source,uint8_t response[64])
{
    uint8_t data[40]={0};
    int rc;
    if(source>=MB86H_CHANNEL_COUNT)return -EINVAL;
    data[0]=(uint8_t)page;
    data[1]=(uint8_t)source;
    rc=mb86H_session_command(session,ops,opaque,MB86H_RESIDENT_COMMAND_DEVICE_QUERY,page,data,response);
    if(rc)return rc;
    if(mb86H_debug_u32be_get(response+12)!=MB86H_RESIDENT_COMMAND_DEVICE_QUERY ||
       mb86H_debug_u32be_get(response+16)!=page || response[24]!=page ||
       response[25]!=source)return -EPROTO;
    rc=(int32_t)mb86H_debug_u32be_get(response+20);
    if(mb86H_debug_u32be_get(response+8)==MB86H_DEBUG_FATAL && rc<0)return rc;
    return mb86H_debug_u32be_get(response+8)==MB86H_DEBUG_OK && !rc ? 0 : -EPROTO;
}
int mb86H_device_query_overview(struct mb86H_session *s,const struct mb86H_session_ops *ops,
    void *ctx,struct mb86H_device_overview *out)
{
    struct mb86H_device_overview d;
    uint8_t response[64];
    unsigned i;
    int rc;
    if(!out)return -EINVAL;
    rc=query(s,ops,ctx,MB86H_DEVICE_QUERY_OVERVIEW,0,response);if(rc)return rc;
    if(response[27])return -EPROTO;
    for(i=2;i<9;++i)if(mb86H_query_word(response+24,i))return -EPROTO;
    mb86H_query_overview_decode(response+24,&d);
    if(d.rf_mask&~((1u<<MB86H_CHANNEL_COUNT)-1u) || d.stream_mask&~d.rf_mask)
        return -EPROTO;
    *out=d;return 0;
}
int mb86H_device_query_source(struct mb86H_session *s,const struct mb86H_session_ops *ops,
    void *ctx,unsigned source,struct mb86H_source_descriptor *out)
{
    struct mb86H_source_descriptor d;
    uint8_t response[64];
    int rc;
    if(!out)return -EINVAL;
    rc=query(s,ops,ctx,MB86H_DEVICE_QUERY_SOURCE,source,response);if(rc)return rc;
    mb86H_query_source_decode(response+24,&d);
    if(d.flags&~(MB86H_DEVICE_QUERY_PRESENT|MB86H_DEVICE_QUERY_STREAM|MB86H_DEVICE_QUERY_AUTO_TSID) ||
       !(d.flags&MB86H_DEVICE_QUERY_PRESENT) || !d.frequency_min ||
       d.frequency_min>d.frequency_max || !d.frequency_step ||
       (d.record_size!=MB86H_TS_PACKET_BYTES && d.record_size!=MB86H_TS_RECORD_BYTES) ||
       (d.stream_format!=MB86H_STREAM_FORMAT_XZM1 && d.stream_format!=MB86H_STREAM_FORMAT_RAW_TS) ||
       mb86H_query_word(response+24,7) || mb86H_query_word(response+24,8))return -EPROTO;
    *out=d;return 0;
}
int mb86H_device_query_state(struct mb86H_session *s,const struct mb86H_session_ops *ops,
    void *ctx,unsigned source,struct mb86H_source_state *out)
{
    struct mb86H_source_state d;
    uint8_t response[64];
    int rc;
    if(!out)return -EINVAL;
    rc=query(s,ops,ctx,MB86H_DEVICE_QUERY_STATE,source,response);if(rc)return rc;
    if(response[27])return -EPROTO;
    mb86H_query_state_decode(response+24,&d);
    if(d.flags&~MB86H_DEVICE_STATE_MASK || d.capture_mask&~((1u<<MB86H_CHANNEL_COUNT)-1u) ||
       d.phase>MB86H_RESIDENT_PHASE_FAILED || d.tsid>0xffffu)return -EPROTO;
    *out=d;return 0;
}
int mb86H_device_query_signal(struct mb86H_session *s,const struct mb86H_session_ops *ops,
    void *ctx,unsigned source,struct mb86H_source_signal *out)
{
    struct mb86H_source_signal d;
    uint8_t response[64];
    unsigned i;
    int rc;
    if(!out)return -EINVAL;
    rc=query(s,ops,ctx,MB86H_DEVICE_QUERY_SIGNAL,source,response);if(rc)return rc;
    if(response[27])return -EPROTO;
    for(i=4;i<9;++i)if(mb86H_query_word(response+24,i))return -EPROTO;
    mb86H_query_signal_decode(response+24,&d);
    if(!d.generation ||
       (d.kind!=MB86H_SIGNAL_TC90512_GR &&
        d.kind!=MB86H_SIGNAL_TC90512_BS && d.kind!=MB86H_SIGNAL_DIB8000) ||
       d.kind==(source?MB86H_SIGNAL_TC90512_GR:MB86H_SIGNAL_TC90512_BS) ||
       (d.kind==MB86H_SIGNAL_DIB8000 && source) ||
       (d.kind!=MB86H_SIGNAL_DIB8000 && d.aux) ||
       (d.kind==MB86H_SIGNAL_DIB8000 &&
        (d.raw>0xffffu || d.aux>0xffffu)))return -EPROTO;
    *out=d;return 0;
}
static int stream_window(struct mb86H_session *s,
    const struct mb86H_session_ops *ops,void *ctx,unsigned source,
    uint32_t generation,uint32_t sequence,unsigned records,int deferred_start)
{
    uint8_t data[40]={0},response[64];
    int32_t firmware_result;
    int rc;
    if(!s || !ops || source>=MB86H_CHANNEL_COUNT || !generation || !records)
        return -EINVAL;
    mb86H_debug_u32be_put(data,generation);
    mb86H_debug_u32be_put(data+4,sequence);
    mb86H_debug_u32be_put(data+8,records);
    rc=mb86H_session_command(s,ops,ctx,MB86H_RESIDENT_COMMAND_STREAM_WINDOW,
        source,data,response);
    if(rc)return rc;
    if(mb86H_debug_u32be_get(response+8)==MB86H_DEBUG_FATAL) {
        rc=(int32_t)mb86H_debug_u32be_get(response+20);
        return rc<0?rc:-EPROTO;
    }
    firmware_result=(int32_t)mb86H_debug_u32be_get(response+20);
    if(mb86H_debug_u32be_get(response+8)!=MB86H_DEBUG_OK ||
       (firmware_result!=0 && firmware_result!=1) ||
       !(mb86H_debug_u32be_get(response+12)&(1u<<source)) ||
       mb86H_debug_u32be_get(response+16)!=sequence ||
       (!response[24] && !(deferred_start && sequence==1 && firmware_result==1)) ||
       !(response[26]&(1u<<source)) ||
       mb86H_debug_u32be_get(response+28)!=generation ||
       mb86H_debug_u32be_get(response+32)!=sequence ||
       mb86H_debug_u32be_get(response+52)!=records)return -EPROTO;
    return 0;
}
int mb86H_device_stream_window(struct mb86H_session *s,
    const struct mb86H_session_ops *ops,void *ctx,unsigned source,
    uint32_t generation,uint32_t sequence,unsigned records)
{return stream_window(s,ops,ctx,source,generation,sequence,records,0);}
int mb86H_device_stream_window_deferred_start(struct mb86H_session *s,
    const struct mb86H_session_ops *ops,void *ctx,unsigned source,
    uint32_t generation,uint32_t sequence,unsigned records)
{return stream_window(s,ops,ctx,source,generation,sequence,records,1);}
