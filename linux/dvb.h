#ifndef MB86H_DVB_H
#define MB86H_DVB_H
#include <linux/device.h>
#include "mb86H/frontend.h"
struct mb86H_dvb;
/* Backend calls serialize semantic FW commands for source 0 (GR) or 1
 * (BS/CS). Firmware owns the source switch. tune() completes only after old
 * source data has been fenced and a matching new sequence session is ready
 * before receive callbacks resume.
 * No register or I2C operations belong in this Linux adapter. */
struct mb86H_dvb_ops {
    int (*tune)(void *,unsigned,const struct mb86H_dvb_request *);
    int (*stream)(void *,unsigned,bool);
    int (*locked)(void *,unsigned,bool *);
    void (*halt)(void *); /* cancel/drain transport callbacks, without calling receive */
};
struct mb86H_dvb *mb86H_dvb_attach(struct device *, const char *name,
                              const struct mb86H_dvb_caps [MB86H_CHANNEL_COUNT],
                              const unsigned int stream_formats[MB86H_CHANNEL_COUNT],
                              const struct mb86H_dvb_ops *, void *);
/* Transport worker/process context only. Each source format is selected
 * explicitly from its descriptor; stream framing is never inferred from
 * payload bytes. */
int mb86H_dvb_receive(struct mb86H_dvb *,unsigned int,const u8 *,size_t);
/* Latch a terminal stream error and wake blocked DVR/demux readers. */
void mb86H_dvb_stream_error(struct mb86H_dvb *,int);
/* Interrupt existing streams without disposing adapter/file ownership. */
void mb86H_dvb_suspend(struct mb86H_dvb *);
void mb86H_dvb_resume(struct mb86H_dvb *);
/* Caller stops new worker calls before detach; halt drains outstanding transport callbacks. */
void mb86H_dvb_detach(struct mb86H_dvb *);
#endif
