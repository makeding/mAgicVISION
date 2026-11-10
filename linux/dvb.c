#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <media/dvb_math.h>
#include <media/dvbdev.h>
#include <media/dvb_frontend.h>
#include <media/dvb_demux.h>
#include <media/dmxdev.h>
#include "dvb.h"
#include "mb86H/device.h"
#include "mb86H/stream_input.h"

struct mb86H_dvb_channel {
    struct mb86H_dvb *owner;
    unsigned source,feeds;
    u32 tuned_frequency;
    u32 tuned_stream_id;
    bool tuned,adapter_registered,frontend_registered,demux_initialized,dmxdev_initialized;
    bool source_selected;
    struct dvb_adapter adapter;
    struct dvb_frontend frontend;
    struct dvb_demux demux;
    struct dmxdev dmxdev;
    struct dmx_frontend hardware;
    bool hardware_added;
};
struct mb86H_dvb {
    struct mutex control,receive;
    const struct mb86H_dvb_ops *ops;
    void *opaque;
    struct mb86H_dvb_caps caps[MB86H_CHANNEL_COUNT];
    struct mb86H_stream_input input;
    unsigned int stream_formats[MB86H_CHANNEL_COUNT];
    struct mb86H_dvb_channel channel;
    bool disconnected;
    bool suspended, reset_required;
    int receive_error;
};
static short adapter_nr[DVB_MAX_ADAPTERS]={[0 ... DVB_MAX_ADAPTERS-1]=-1};

/* TC90512 monitor equations use the same registers and fixed-point form as
 * the in-kernel TC90522 driver. Values are milli-dB, not raw AGC units. */
static bool signal_cnr(const struct mb86H_source_signal *s,s64 *mdb)
{
    s64 cn;
    u32 p,tmp,raw=s->raw;
    if(s->kind==MB86H_SIGNAL_TC90512_GR) {
        if(!raw || raw>5505024u)return false;
        p=(intlog10(5505024u)-intlog10(raw))*10u;
        cn=24772;
        cn+=div64_s64(43827LL*p,10)>>24;
        tmp=p>>8;cn+=div64_s64(3184LL*tmp*tmp,10)>>32;
        tmp=p>>13;cn-=div64_s64(128LL*tmp*tmp*tmp,10)>>33;
        tmp=p>>18;cn+=div64_s64(192LL*tmp*tmp*tmp*tmp,1000)>>24;
        *mdb=cn>>3;return true;
    }
    if(s->kind==MB86H_SIGNAL_TC90512_BS) {
        u32 p4;
        if(raw<3000u || raw>0xffffu)return false;
        raw-=3000u;
        p=int_sqrt(raw<<16);p4=raw*raw;
        cn=div64_s64(-16346LL*p4*p,10)>>35;
        cn+=(14341LL*p4)>>21;
        cn-=(50259LL*raw*p)>>23;
        cn+=(88977LL*raw)>>9;
        cn-=(89565LL*p)>>11;
        cn+=58857LL<<3;
        *mdb=cn>>3;return true;
    }
    if(s->kind==MB86H_SIGNAL_DIB8000) {
        u32 noise=(s->raw>>6)&0xffu,carrier=(s->aux>>6)&0xffu;
        int ne=(s->raw&0x3fu),se=(s->aux&0x3fu);
        if(!noise || !carrier || s->raw>0xffffu || s->aux>0xffffu)return false;
        if(ne&0x20)ne-=0x40;
        if(se&0x20)se-=0x40;
        cn=div64_s64(((s64)intlog10(carrier)-(s64)intlog10(noise))*10000LL,
            16777216LL)+(s64)(se-ne)*3010LL;
        if(cn<-100000 || cn>100000)return false;
        *mdb=cn;return true;
    }
    return false;
}

static int get_frontend(struct dvb_frontend *fe,struct dtv_frontend_properties *p)
{
    struct mb86H_dvb_channel *c=fe->demodulator_priv;
    struct mb86H_dvb *d=c->owner;
    struct mb86H_source_signal sample={0};
    s64 cnr;
    int rc=0;
    p->cnr.len=1;
    p->cnr.stat[0].scale=FE_SCALE_NOT_AVAILABLE;
    p->cnr.stat[0].svalue=0;
    mutex_lock(&d->control);
    if(d->disconnected)rc=-ENODEV;
    else if(d->suspended)rc=-EHOSTDOWN;
    else if(c->source_selected && c->tuned &&
            p->frequency==c->tuned_frequency &&
            p->delivery_system==(c->source?SYS_ISDBS:SYS_ISDBT) &&
            (!c->source || p->stream_id==c->tuned_stream_id) &&
            d->ops->signal) {
        rc=d->ops->signal(d->opaque,c->source,&sample);
        if(rc==MB86H_FE_UNSUPPORTED)rc=0;
        if(!rc && sample.kind && signal_cnr(&sample,&cnr)) {
            p->cnr.stat[0].svalue=cnr;
            p->cnr.stat[0].scale=FE_SCALE_DECIBEL;
        }
    }
    mutex_unlock(&d->control);
    return rc;
}

static void frontend_frequency_limits(const struct mb86H_dvb_caps caps[MB86H_CHANNEL_COUNT],
                                      u32 *minimum, u32 *maximum)
{
    u64 min = ~(u64)0, max = 0;
    unsigned i;
    for (i = 0; i < MB86H_CHANNEL_COUNT; ++i) {
        u64 scale = i ? 1000u : 1u;
        u64 low, high;
        if (!caps[i].present) continue;
        low = (u64)caps[i].frequency_min * scale;
        high = (u64)caps[i].frequency_max * scale;
        if (low < min) min = low;
        if (high > max) max = high;
    }
    *minimum = min > (u64)~(u32)0 ? ~(u32)0 : (u32)min;
    *maximum = max > (u64)~(u32)0 ? ~(u32)0 : (u32)max;
}

static int set_frontend(struct dvb_frontend *fe)
{
    struct mb86H_dvb_channel *c=fe->demodulator_priv;
    struct mb86H_dvb *d=c->owner;
    struct dtv_frontend_properties *p=&fe->dtv_property_cache;
    unsigned source;
    struct mb86H_dvb_request r={
        .system=MB86H_DVB_GR,
        .frequency=p->frequency,.bandwidth_hz=p->bandwidth_hz,
        .stream_id=p->stream_id,.symbol_rate=p->symbol_rate,
        .layers=p->isdbt_layer_enabled,.partial_reception=p->isdbt_partial_reception,
        .sound_broadcasting=p->isdbt_sb_mode
    };
    int rc;
    if(p->delivery_system==SYS_ISDBT)source=0;
    else if(p->delivery_system==SYS_ISDBS)source=1;
    else return -EINVAL;
    r.system=source?MB86H_DVB_BSCS:MB86H_DVB_GR;
    /* stream_id is an ISDB-S TSID. The shared DVB property cache can keep
     * it across a delivery-system switch; it has no meaning for ISDB-T. */
    if(!source)r.stream_id=(u32)-1;
    dev_info(c->adapter.device,"DVB adapter%d frontend0: tune request source=%u frequency=%u stream_id=%x layers=%u\n",
             c->adapter.num,source,r.frequency,r.stream_id,r.layers);
    if(!mb86H_dvb_request_valid(d->caps+source,&r)) {
        dev_err(c->adapter.device,"DVB adapter%d frontend0: invalid tune source=%u frequency=%u bandwidth=%u stream_id=%x layers=%u partial=%u sound=%u\n",
               c->adapter.num,source,r.frequency,r.bandwidth_hz,r.stream_id,r.layers,
               r.partial_reception,r.sound_broadcasting);
        return -EINVAL;
    }
    mutex_lock(&d->control);
    if(d->disconnected){rc=-ENODEV;goto out;}
    if(d->suspended){rc=-EHOSTDOWN;goto out;}
    /* Pre-sleep demux feeds are terminal. A frontend auto-retune must not
     * silently revive them; their owners must close before a fresh tune. */
    if(d->reset_required && c->feeds){rc=-EPIPE;goto out;}
    if(c->feeds && (!c->source_selected || c->source!=source)){
        rc=-EBUSY;goto out;
    }
    mutex_lock(&d->receive);c->tuned=false;mutex_unlock(&d->receive);
    if(c->feeds && !d->reset_required) {
        rc=d->ops->stream(d->opaque,c->source,false);
        if(rc)goto out;
    }
    rc=d->ops->tune(d->opaque,source,&r);
    mutex_lock(&d->receive);
    if(!rc){
        c->source=source;c->source_selected=true;c->tuned_frequency=r.frequency;
        c->tuned_stream_id=r.stream_id;
        rc=mb86H_stream_input_init(&d->input,d->stream_formats[source],source,
                                   d->caps[source].record_size);
        d->receive_error=0;
        c->tuned=!rc;
        d->reset_required=false;
    }
    mutex_unlock(&d->receive);
    if(!rc && c->feeds) {
        rc=d->ops->stream(d->opaque,c->source,true);
        if(rc) {
            mutex_lock(&d->receive);c->tuned=false;mutex_unlock(&d->receive);
        }
    }
out:
    if(rc)dev_err(c->adapter.device,"DVB adapter%d frontend0: tune source=%u frequency=%u failed=%d\n",
                  c->adapter.num,source,r.frequency,rc);
    mutex_unlock(&d->control);return rc;
}
static int read_status(struct dvb_frontend *fe,enum fe_status *status)
{
    struct mb86H_dvb_channel *c=fe->demodulator_priv;
    struct mb86H_dvb *d=c->owner;
    bool locked=false;
    int rc;
    *status=0;
    mutex_lock(&d->control);
    if(d->disconnected)rc=-ENODEV;
    else if(d->suspended)rc=-EHOSTDOWN;
    else if(!c->source_selected || !c->tuned)rc=0;
    else rc=d->ops->locked(d->opaque,c->source,&locked);
    if(!rc && locked)*status=FE_HAS_SIGNAL|FE_HAS_CARRIER|FE_HAS_VITERBI|FE_HAS_SYNC|FE_HAS_LOCK;
    mutex_unlock(&d->control);return rc;
}
static enum dvbfe_algo frontend_algo(struct dvb_frontend *fe)
{
    return DVBFE_ALGO_HW;
}
static int frontend_tune(struct dvb_frontend *fe,bool re_tune,
                         unsigned int mode_flags,unsigned int *delay,
                         enum fe_status *status)
{
    int rc;
    *delay=msecs_to_jiffies(100);
    *status=0;
    if(re_tune) {
        rc=set_frontend(fe);
        if(rc)return rc;
    }
    return read_status(fe,status);
}
static int start_feed(struct dvb_demux_feed *feed)
{
    struct mb86H_dvb_channel *c=feed->demux->priv;
    struct mb86H_dvb *d=c->owner;
    int rc=0;
    mutex_lock(&d->control);
    if(d->disconnected)rc=-ENODEV;
    else if(d->suspended)rc=-EHOSTDOWN;
    else if(!c->tuned)rc=-EAGAIN;
    else if(!c->feeds) {
        /* STOP drains the previous receiver; each firmware capture restarts
         * its mux sequence. Reset before the new RX can deliver anything. */
        mutex_lock(&d->receive);
        rc=mb86H_stream_input_init(&d->input,d->stream_formats[c->source],c->source,
                                   d->caps[c->source].record_size);
        d->receive_error=0;
        mutex_unlock(&d->receive);
        if(!rc)rc=d->ops->stream(d->opaque,c->source,true);
    }
    if(!rc)c->feeds++;
    mutex_unlock(&d->control);return rc;
}
static int stop_feed(struct dvb_demux_feed *feed)
{
    struct mb86H_dvb_channel *c=feed->demux->priv;
    struct mb86H_dvb *d=c->owner;
    int rc=0;
    mutex_lock(&d->control);
    if(!c->feeds)rc=-EINVAL;
    else if(--c->feeds==0 && !d->disconnected && !d->suspended &&
            !d->reset_required)rc=d->ops->stream(d->opaque,c->source,false);
    mutex_unlock(&d->control);return rc;
}
static int packet(void *opaque,const u8 *p)
{
    struct mb86H_dvb_channel *c=opaque;
    dvb_dmx_swfilter_packets(&c->demux,p,1);return 0;
}
int mb86H_dvb_receive(struct mb86H_dvb *d,unsigned source,const u8 *data,size_t size)
{
    int rc;
    size_t desyncs;
    if(!d)return -EINVAL;
    mutex_lock(&d->receive);
    if(d->disconnected)rc=-ENODEV;
    else if(d->receive_error)rc=d->receive_error;
    else {
        desyncs=d->input.framer.desyncs;
        if(!d->channel.tuned || !d->channel.source_selected)rc=0;
        else rc=mb86H_stream_input_push(&d->input,source,data,size,
                                        packet,&d->channel);
        if(d->input.framer.desyncs!=desyncs)
            dev_warn_ratelimited(d->channel.adapter.device,
                "DVB adapter%d frontend0: TS sync lost; resynchronizing (desyncs=%zu)\n",
                d->channel.adapter.num,d->input.framer.desyncs);
        if(rc) {
            dev_err_ratelimited(d->channel.adapter.device,"DVB adapter%d frontend0: TS framing failed protocol_status=%d\n",
                                d->channel.adapter.num,rc);
            rc=d->receive_error=-EILSEQ;
        }
    }
    mutex_unlock(&d->receive);return rc;
}

void mb86H_dvb_stream_error(struct mb86H_dvb *d,int error)
{
    struct dmxdev *dmx;
    unsigned long flags;
    unsigned i;
    if(!d || error>=0)return;
    mutex_lock(&d->receive);
    if(d->disconnected)goto out;
    if(!d->receive_error)d->receive_error=error;
    dmx=&d->channel.dmxdev;
    spin_lock_irqsave(&dmx->lock,flags);
    dmx->dvr_buffer.error=d->receive_error;
    for(i=0;i<dmx->filternum;++i) {
        struct dmxdev_filter *filter=dmx->filter+i;
        if(filter->state==DMXDEV_STATE_GO) {
            filter->buffer.error=d->receive_error;
            wake_up(&filter->buffer.queue);
        }
    }
    spin_unlock_irqrestore(&dmx->lock,flags);
    wake_up(&dmx->dvr_buffer.queue);
out:
    mutex_unlock(&d->receive);
}

void mb86H_dvb_suspend(struct mb86H_dvb *d)
{
    if(!d)return;
    mutex_lock(&d->control);
    mutex_lock(&d->receive);
    d->suspended=true;d->reset_required=true;d->channel.tuned=false;
    mb86H_stream_input_reset(&d->input);
    mutex_unlock(&d->receive);
    mutex_unlock(&d->control);
    mb86H_dvb_stream_error(d,-EHOSTDOWN);
}

void mb86H_dvb_resume(struct mb86H_dvb *d)
{
    if(!d)return;
    mutex_lock(&d->control);
    d->suspended=false;
    mutex_unlock(&d->control);
}

static void release_channel(struct mb86H_dvb_channel *c)
{
    if(c->frontend_registered) {
        dvb_unregister_frontend(&c->frontend);c->frontend_registered=false;
    }
    if(c->dmxdev_initialized){dvb_dmxdev_release(&c->dmxdev);c->dmxdev_initialized=false;}
    if(c->hardware_added){c->demux.dmx.remove_frontend(&c->demux.dmx,&c->hardware);c->hardware_added=false;}
    if(c->demux_initialized){dvb_dmx_release(&c->demux);c->demux_initialized=false;}
    if(c->adapter_registered){dvb_unregister_adapter(&c->adapter);c->adapter_registered=false;}
}
struct mb86H_dvb *mb86H_dvb_attach(struct device *parent, const char *name,
                              const struct mb86H_dvb_caps caps[MB86H_CHANNEL_COUNT],
                              const unsigned int stream_formats[MB86H_CHANNEL_COUNT],
                              const struct mb86H_dvb_ops *ops, void *opaque)
{
    struct mb86H_dvb *d;
    int rc;
    unsigned initial_source;
    if(!parent || !name || !ops || !ops->tune || !ops->stream || !ops->locked || !ops->halt ||
       !mb86H_dvb_caps_valid(caps) || !stream_formats)
        return ERR_PTR(-EINVAL);
    for (unsigned source = 0; source < MB86H_CHANNEL_COUNT; ++source)
        if (caps[source].present &&
            stream_formats[source] != MB86H_STREAM_FORMAT_XZM1 &&
            stream_formats[source] != MB86H_STREAM_FORMAT_RAW_TS)
            return ERR_PTR(-EINVAL);
    d=kzalloc(sizeof(*d),GFP_KERNEL);if(!d)return ERR_PTR(-ENOMEM);
    mutex_init(&d->control);mutex_init(&d->receive);
    d->ops=ops;d->opaque=opaque;
    memcpy(d->stream_formats,stream_formats,sizeof(d->stream_formats));
    memcpy(d->caps,caps,sizeof(d->caps));
    initial_source=caps[0].present?0:1;
    rc=mb86H_stream_input_init(&d->input,d->stream_formats[initial_source],initial_source,
                               caps[initial_source].record_size);
    if(rc)goto fail;
    {
        struct mb86H_dvb_channel *c=&d->channel;
        c->owner=d;c->source=0;
        rc=dvb_register_adapter(&c->adapter,name,THIS_MODULE,parent,adapter_nr);
        if(rc<0)goto fail;
        c->adapter_registered=true;
        c->demux.priv=c;c->demux.filternum=256;c->demux.feednum=256;
        c->demux.dmx.capabilities=DMX_TS_FILTERING|DMX_SECTION_FILTERING;
        c->demux.start_feed=start_feed;c->demux.stop_feed=stop_feed;
        rc=dvb_dmx_init(&c->demux);if(rc)goto fail;c->demux_initialized=true;
        c->dmxdev.filternum=256;c->dmxdev.demux=&c->demux.dmx;
        rc=dvb_dmxdev_init(&c->dmxdev,&c->adapter);if(rc)goto fail;c->dmxdev_initialized=true;
        c->hardware.source=DMX_FRONTEND_0;
        rc=c->demux.dmx.add_frontend(&c->demux.dmx,&c->hardware);if(rc)goto fail;c->hardware_added=true;
        rc=c->demux.dmx.connect_frontend(&c->demux.dmx,&c->hardware);if(rc)goto fail;
        c->frontend.demodulator_priv=c;
        {
            unsigned n = 0;
            if (caps[0].present) c->frontend.ops.delsys[n++] = SYS_ISDBT;
            if (caps[1].present) c->frontend.ops.delsys[n++] = SYS_ISDBS;
        }
        strscpy(c->frontend.ops.info.name,name,sizeof(c->frontend.ops.info.name));
        frontend_frequency_limits(caps,&c->frontend.ops.info.frequency_min_hz,
                                  &c->frontend.ops.info.frequency_max_hz);
        c->frontend.ops.set_frontend=set_frontend;c->frontend.ops.read_status=read_status;
        c->frontend.ops.get_frontend=get_frontend;
        c->frontend.ops.get_frontend_algo=frontend_algo;
        c->frontend.ops.tune=frontend_tune;
        rc=dvb_register_frontend(&c->adapter,&c->frontend);if(rc)goto fail;c->frontend_registered=true;
    }
    return d;
fail:
    mutex_lock(&d->control);mutex_lock(&d->receive);
    d->disconnected=true;
    mutex_unlock(&d->receive);mutex_unlock(&d->control);
    d->ops->halt(d->opaque);
    release_channel(&d->channel);
    kfree(d);return ERR_PTR(rc);
}
void mb86H_dvb_detach(struct mb86H_dvb *d)
{
    if(!d)return;
    mutex_lock(&d->control);mutex_lock(&d->receive);
    d->disconnected=true;
    mutex_unlock(&d->receive);mutex_unlock(&d->control);
    d->ops->halt(d->opaque);
    release_channel(&d->channel);
    kfree(d);
}
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("mb86H Series ISDB-T/ISDB-S TV tuner driver");
