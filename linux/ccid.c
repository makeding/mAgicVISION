// SPDX-License-Identifier: GPL-2.0
/* Standard USB CCID gadget frontend for the mb86H physical card backend. */
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/idr.h>
#include <linux/list.h>

#include "ccid.h"
#include "mb86H/card.h"

#define CCID_VID 0x1209
#define CCID_PID 0x0001
#define CCID_HEADER 10u
#define CCID_OUT_BYTES (CCID_HEADER + MB86H_CARD_MAX_TX)
#define CCID_IN_BYTES (CCID_HEADER + MB86H_CARD_MAX_RX)
#define CCID_OUT_PACKET 512u
#define CCID_PRESENCE_INTERVAL_MS 250u
#define CCID_EXTENSION_INTERVAL_MS 1000u
#define CCID_FINAL_RETRY_MS 20u
#define CCID_FINAL_MAX_RETRIES 3u
#define CCID_FINAL_MAX_ATTEMPTS (1u + CCID_FINAL_MAX_RETRIES)

#define PC_TO_RDR_SET_PARAMETERS 0x61
#define PC_TO_RDR_POWER_ON 0x62
#define PC_TO_RDR_POWER_OFF 0x63
#define PC_TO_RDR_GET_SLOT_STATUS 0x65
#define PC_TO_RDR_XFR_BLOCK 0x6f
#define PC_TO_RDR_GET_PARAMETERS 0x6c
#define PC_TO_RDR_RESET_PARAMETERS 0x6d
#define PC_TO_RDR_ABORT 0x72
#define RDR_TO_PC_DATABLOCK 0x80
#define RDR_TO_PC_SLOTSTATUS 0x81
#define RDR_TO_PC_PARAMETERS 0x82

#define CCID_STATUS_OK 0x00
#define CCID_STATUS_FAILED 0x40
#define CCID_ERROR_CMD_NOT_SUPPORTED 0x00
#define CCID_ERROR_BAD_LENGTH 0x01
#define CCID_ERROR_BAD_SLOT 0x05
#define CCID_ERROR_BAD_PROTOCOL 0x07
#define CCID_ERROR_CMD_ABORTED 0xff
#define CCID_ERROR_SLOT_BUSY 0xe0
#define CCID_ERROR_HW_ERROR 0xfb

struct ccid_function {
	struct usb_function function;
	struct usb_ep *in_ep, *out_ep, *intr_ep;
	struct usb_request *in_req, *out_req, *intr_req;
	struct work_struct work, io_work;
	struct delayed_work slot_work;
	struct delayed_work extension_work;
	struct mutex backend_lock;
	spinlock_t state_lock, submit_lock;
	const struct mb86H_ccid_ops *ops;
	void *opaque;
	bool registered, online, out_queued, in_flight, intr_queued, slot_change_pending;
	atomic_t abort_token;
	u8 interface_id, slot_change_value;
	u32 generation, command_generation, parameters_generation;
	u32 backend_generation, command_backend_generation;
	u8 frame[CCID_OUT_BYTES];
	u8 command[CCID_OUT_BYTES];
	size_t frame_len, expected_len;
	bool work_pending, parameters_valid, in_pending;
	bool extension_active, final_pending;
	u8 extension_type, extension_slot, extension_seq, extension_multiplier,extension_status;
	u8 in_owner, final_attempts;
	u32 command_epoch, command_work_epoch;
	u32 extension_generation, extension_backend_generation, extension_epoch;
	u32 final_generation, final_epoch;
	size_t final_length;
	u8 final[CCID_IN_BYTES];
	int presence_error;
	u8 parameters[7], protocol, parameters_length,cached_icc;
	bool cached_icc_valid;
};

struct ccid_reader {
    struct list_head node;
    struct ccid_function ccid;
    struct usb_composite_driver driver;
    struct usb_configuration config;
    struct usb_device_descriptor device;
    struct usb_string strings[4];
    struct usb_gadget_strings language;
    struct usb_gadget_strings *string_tables[2];
    char driver_name[32], udc_name[32];
    char *product, *serial;
    int id;
};
static LIST_HEAD(ccid_readers);
static DEFINE_IDA(ccid_ids);
static DEFINE_MUTEX(ccid_lifecycle_lock);
static char *ccid_udc = "dummy_udc.0";
module_param_named(ccid_udc, ccid_udc, charp, 0444);
MODULE_PARM_DESC(ccid_udc, "first dummy_hcd UDC; each physical reader uses its own controller");

static inline struct ccid_function *func_to_ccid(struct usb_function *f)
{
	return container_of(f, struct ccid_function, function);
}

static u32 ccid_get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
		((u32)p[3] << 24);
}

static void ccid_put_le32(u8 *p, u32 n)
{
	p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24;
}

/*
 * dummy_hcd can invoke complete before usb_ep_queue returns.  Only io_work
 * submits requests; completions update state and schedule work, never submit
 * recursively.  submit_lock serializes queue versus endpoint disable, but
 * completions take only state_lock.  Lock order is submit_lock -> state_lock.
 */
enum ccid_endpoint { CCID_INTR, CCID_IN, CCID_OUT };
enum ccid_in_owner { CCID_IN_NONE, CCID_IN_EXTENSION, CCID_IN_FINAL };

static void ccid_io_submit(struct ccid_function *c, enum ccid_endpoint which)
{
    struct usb_ep *ep = NULL;
    struct usb_request *req = NULL;
    unsigned long flags;
    int rc;
    bool retry_final = false,final_exhausted=false;

    spin_lock_irqsave(&c->submit_lock, flags);
    spin_lock(&c->state_lock);
    if (!c->online)
        goto unlock_state;
    switch (which) {
    case CCID_INTR:
        if (!c->slot_change_pending || c->intr_queued)
            break;
        ep = c->intr_ep; req = c->intr_req;
        ((u8 *)req->buf)[0] = 0x50;
        ((u8 *)req->buf)[1] = c->slot_change_value;
        req->length = 2;
        c->intr_queued = true;
        c->slot_change_pending = false;
        break;
    case CCID_IN:
        if (!c->in_pending)
            break;
        ep = c->in_ep; req = c->in_req;
        c->in_pending = false;
        break;
    case CCID_OUT:
        if (c->out_queued || c->work_pending || c->in_flight)
            break;
        ep = c->out_ep; req = c->out_req;
        req->length = CCID_OUT_PACKET;
        req->zero = 0;
        c->out_queued = true;
        break;
    }
unlock_state:
    spin_unlock(&c->state_lock);
    if (req) {
        /* No state_lock across this call: complete may run inline. */
        rc = usb_ep_queue(ep, req, GFP_ATOMIC);
        if (rc) {
            spin_lock(&c->state_lock);
            switch (which) {
            case CCID_INTR:
                c->intr_queued = false;
                c->slot_change_pending = true;
                break;
            case CCID_IN:
                c->in_flight = false;
                c->in_owner = CCID_IN_NONE;
                if (c->online && c->final_pending &&
                    c->final_generation == c->generation &&
                    c->final_epoch == c->command_work_epoch) {
                    if(c->final_attempts<CCID_FINAL_MAX_ATTEMPTS)retry_final=true;
                    else final_exhausted=true;
                }
                break;
            case CCID_OUT:
                c->out_queued = false;
                break;
            }
            spin_unlock(&c->state_lock);
        }
    }
    spin_unlock_irqrestore(&c->submit_lock, flags);
    if (retry_final)
        mod_delayed_work(system_wq, &c->extension_work,
                         msecs_to_jiffies(CCID_FINAL_RETRY_MS));
    if(final_exhausted)
        pr_err_ratelimited("mb86H_ccid: final IN queue failed after %u attempts; command fenced\n",
                           CCID_FINAL_MAX_ATTEMPTS);
}

static void ccid_io_work(struct work_struct *work)
{
    struct ccid_function *c = container_of(work, struct ccid_function, io_work);

    ccid_io_submit(c, CCID_INTR);
    ccid_io_submit(c, CCID_IN);
    ccid_io_submit(c, CCID_OUT);
}

static void ccid_submit_out(struct ccid_function *c, gfp_t unused)
{
    unsigned long flags;
    (void)unused;
    spin_lock_irqsave(&c->state_lock, flags);
    if (c->online)
        schedule_work(&c->io_work);
    spin_unlock_irqrestore(&c->state_lock, flags);
}

static void ccid_notify_slot_change(struct ccid_function *c, gfp_t unused)
{
    unsigned long flags;
    (void)unused;
    spin_lock_irqsave(&c->state_lock, flags);
    c->slot_change_pending = true;
    if (c->online)
        schedule_work(&c->io_work);
    spin_unlock_irqrestore(&c->state_lock, flags);
}

static void ccid_slot_work(struct work_struct *work)
{
    struct ccid_function *c = container_of(to_delayed_work(work),
                                          struct ccid_function, slot_work);
    unsigned long flags;
    u8 status = MB86H_CARD_ABSENT;
    int rc = 0;

    mutex_lock(&c->backend_lock);
    spin_lock_irqsave(&c->state_lock, flags);
    if (!c->online) {
        spin_unlock_irqrestore(&c->state_lock, flags);
        mutex_unlock(&c->backend_lock);
        return;
    }
    spin_unlock_irqrestore(&c->state_lock, flags);
    if (c->registered && c->ops)
        rc = c->ops->status(c->opaque, &status);
    if (!rc && status > MB86H_CARD_ABSENT)
        rc = -EPROTO;
    if (rc != c->presence_error) {
        if (rc)
            pr_warn("mb86H_ccid: card presence query failed: %d\n", rc);
        c->presence_error = rc;
    }

    spin_lock_irqsave(&c->state_lock, flags);
    if (c->online) {
        if (!rc && status <= MB86H_CARD_ABSENT) {
            c->cached_icc=status;c->cached_icc_valid=true;
            u8 value = status == MB86H_CARD_ABSENT ? 2 : 3;
            if (value != c->slot_change_value) {
                c->slot_change_value = value;
                c->slot_change_pending = true;
                schedule_work(&c->io_work);
            }
        }
        /* The physical reader has no host interrupt for card detect.
         * Poll the existing semantic STATUS; failures never imply removal. */
        if (c->registered)
            schedule_delayed_work(&c->slot_work,
                msecs_to_jiffies(CCID_PRESENCE_INTERVAL_MS));
    }
    spin_unlock_irqrestore(&c->state_lock, flags);
    mutex_unlock(&c->backend_lock);
}

static void ccid_reply(u8 *response, u8 type, u8 slot, u8 seq, u8 status,
                       u8 error, u8 specific, const u8 *data, size_t length);
static bool ccid_final_current(const struct ccid_function *c)
{
    return c->final_pending && c->online &&
        c->final_generation == c->generation &&
        c->final_epoch == c->command_work_epoch;
}
static bool ccid_stage_final_locked(struct ccid_function *c)
{
    if (!ccid_final_current(c) || c->in_flight ||
        c->final_attempts >= CCID_FINAL_MAX_ATTEMPTS)
        return false;
    memcpy(c->in_req->buf, c->final, c->final_length);
    c->in_req->length = c->final_length;
    c->in_req->zero = 1;
    ++c->final_attempts;
    c->in_owner = CCID_IN_FINAL;
    c->in_flight = true;
    c->in_pending = true;
    return true;
}
/* A time-extension never completes the command.  The final packet is held
 * while its IN transfer is in flight and wins the next completion. */
static int ccid_queue_final(struct ccid_function *c,const u8 *data,size_t length,
                            u32 generation,u32 epoch)
{
    unsigned long flags;
    bool queue=false,release=false;
    int rc=0;
    spin_lock_irqsave(&c->state_lock,flags);
    if(c->extension_epoch==epoch)c->extension_active=false;
    if(length>CCID_IN_BYTES) {
        rc=-EMSGSIZE;
    } else if(!c->online || generation!=c->generation ||
              epoch!=c->command_work_epoch) {
        if(c->work_pending && epoch==c->command_work_epoch)
            c->work_pending=false;
        release=c->online && !c->work_pending;
        rc=-ESHUTDOWN;
    } else {
        memcpy(c->final,data,length);
        c->final_length=length;
        c->final_generation=generation;
        c->final_epoch=epoch;
        c->final_attempts=0;
        c->final_pending=true;
        queue=ccid_stage_final_locked(c);
    }
    spin_unlock_irqrestore(&c->state_lock,flags);
    /* Retire this command's periodic timer before its final response can
     * complete and admit another command.  This prevents a running stale
     * timer from observing the next command's fields. */
    cancel_delayed_work_sync(&c->extension_work);
    if(queue)schedule_work(&c->io_work);
    if(release)ccid_submit_out(c,GFP_ATOMIC);
    return rc;
}
static void ccid_extension_work(struct work_struct *work)
{
    struct ccid_function *c=container_of(to_delayed_work(work),struct ccid_function,extension_work);
    u8 reply[CCID_HEADER];unsigned long flags;bool queue=false,rearm=false;
    spin_lock_irqsave(&c->state_lock,flags);
    if(ccid_final_current(c)) {
        queue=ccid_stage_final_locked(c);
    } else if(c->online && c->extension_active &&
              c->extension_generation==c->generation &&
              c->extension_backend_generation==c->backend_generation &&
              c->extension_epoch==c->command_work_epoch) {
        if(!c->in_flight) {
            ccid_reply(reply,c->extension_type,c->extension_slot,c->extension_seq,
                       c->extension_status,c->extension_multiplier,0,NULL,0);
            memcpy(c->in_req->buf,reply,sizeof(reply));
            c->in_req->length=sizeof(reply);c->in_req->zero=1;
            c->in_owner=CCID_IN_EXTENSION;
            c->in_flight=true;c->in_pending=true;queue=true;
        }
        rearm=true;
    } else {
        c->extension_active=false;
    }
    spin_unlock_irqrestore(&c->state_lock,flags);
    if(queue)schedule_work(&c->io_work);
    if(rearm)
        mod_delayed_work(system_wq,&c->extension_work,
                         msecs_to_jiffies(CCID_EXTENSION_INTERVAL_MS));
}
static void ccid_extension_begin(struct ccid_function *c,u8 type,u8 slot,u8 seq,
                                 u32 generation,u32 backend,u32 epoch)
{
    unsigned long flags;
    spin_lock_irqsave(&c->state_lock,flags);
    c->extension_active=true;c->extension_type=type;
    c->extension_slot=slot;c->extension_seq=seq;c->extension_multiplier=1;
    c->extension_status=0x80u|
        (c->cached_icc_valid?c->cached_icc:MB86H_CARD_ABSENT);
    c->extension_generation=generation;c->extension_backend_generation=backend;
    c->extension_epoch=epoch;
    spin_unlock_irqrestore(&c->state_lock,flags);
    mod_delayed_work(system_wq,&c->extension_work,
                     msecs_to_jiffies(CCID_EXTENSION_INTERVAL_MS));
}

static void ccid_reply(u8 *response, u8 type, u8 slot, u8 seq, u8 status,
		       u8 error, u8 specific, const u8 *data, size_t length)
{
	response[0] = type;
	ccid_put_le32(response + 1, length);
	response[5] = slot;
	response[6] = seq;
	response[7] = status;
	response[8] = error;
	response[9] = specific;
	if (length)
		memcpy(response + CCID_HEADER, data, length);
}

static u8 ccid_response_type(u8 command)
{
	switch (command) {
	case PC_TO_RDR_POWER_ON:
	case PC_TO_RDR_XFR_BLOCK:
		return RDR_TO_PC_DATABLOCK;
	case PC_TO_RDR_GET_PARAMETERS:
	case PC_TO_RDR_SET_PARAMETERS:
	case PC_TO_RDR_RESET_PARAMETERS:
		return RDR_TO_PC_PARAMETERS;
	default:
		return RDR_TO_PC_SLOTSTATUS;
	}
}

static void ccid_cache_icc(struct ccid_function *c,u8 status)
{
    unsigned long flags;
    spin_lock_irqsave(&c->state_lock,flags);
    c->cached_icc=status;c->cached_icc_valid=true;
    spin_unlock_irqrestore(&c->state_lock,flags);
}

static u8 ccid_status_locked(struct ccid_function *c, int *result)
{
	u8 status = MB86H_CARD_ABSENT;
	if (!c->registered || !c->ops) {
		*result = 0;
		return status;
	}
	*result = c->ops->status(c->opaque, &status);
	if (status > MB86H_CARD_ABSENT) {
		*result = -EPROTO;
		return MB86H_CARD_ABSENT;
	}
	if (*result)
		return MB86H_CARD_ABSENT;
	ccid_cache_icc(c,status);
    if(status!=MB86H_CARD_ACTIVE)c->parameters_valid=false;
	return status;
}

static u8 ccid_error_from_errno(int rc)
{
	if (rc == -EOPNOTSUPP)
		return CCID_ERROR_CMD_NOT_SUPPORTED;
    if (rc == -EPROTONOSUPPORT) return 0xf6;
	if (rc == -ETIMEDOUT) return 0xfe;
	return CCID_ERROR_HW_ERROR;
}

static bool ccid_reserved_zero(const u8 *frame)
{
	return !frame[8] && !frame[9];
}

/* CCID's parameter record is a wire encoding of neutral core metadata. */
static int ccid_parameters(struct ccid_function *c,const u8 *atr,size_t length)
{
    struct mb86H_card_parameters p;
    int rc=mb86H_card_parse_atr(atr,length,&p);
    if(rc)return rc;
    c->protocol=p.protocol;c->parameters_length=0;
    if(p.protocol>1)return 0; /* ATR remains readable; exchange is unsupported. */
    c->parameters[0]=p.fi_di;
    c->parameters[1]=(p.inverse?2:0)|(p.protocol?0x10|p.crc:0);
    c->parameters[2]=p.guard;
    c->parameters[3]=p.protocol?(p.bwi<<4)|p.cwi:p.wi;
    c->parameters[4]=0;
    c->parameters[5]=p.ifsc;c->parameters[6]=0;
    c->parameters_length=p.protocol?7:5;
    return 0;
}

/* backend_lock is held by the caller; completion snapshots this epoch
 * under state_lock so queued commands cannot cross a physical rebind. */
static void ccid_backend_changed(struct ccid_function *c)
{
    unsigned long flags;
    spin_lock_irqsave(&c->state_lock, flags);
    ++c->backend_generation;
    c->frame_len = c->expected_len = 0;
    c->parameters_valid = false;
    c->presence_error = 0;
    c->extension_active=false;
    c->cached_icc=MB86H_CARD_ABSENT;c->cached_icc_valid=true;
    spin_unlock_irqrestore(&c->state_lock, flags);
}

static void ccid_work(struct work_struct *work)
{
	struct ccid_function *c = container_of(work, struct ccid_function, work);
	const u8 *frame = c->command;
	u32 generation = READ_ONCE(c->command_generation);
    u32 backend_generation = READ_ONCE(c->command_backend_generation);
	u32 epoch = READ_ONCE(c->command_work_epoch);
	u8 response[CCID_IN_BYTES], atr[MB86H_CARD_MAX_ATR], rx[MB86H_CARD_MAX_RX];
	u8 command, slot, seq, icc = MB86H_CARD_ABSENT;
	u32 length;
	size_t out_length;
	int rc = 0;
    unsigned long flags;
	bool release,extend;

    command=frame[0];slot=frame[5];seq=frame[6];
    length=ccid_get_le32(frame+1);
    spin_lock_irqsave(&c->state_lock, flags);
	if (!c->online || generation != c->generation || epoch != c->command_work_epoch) {
        if(c->work_pending && epoch==c->command_work_epoch)c->work_pending=false;
        release=c->online && !c->work_pending;
        spin_unlock_irqrestore(&c->state_lock, flags);
		if(release)ccid_submit_out(c,GFP_ATOMIC);
		return;
	}
    spin_unlock_irqrestore(&c->state_lock, flags);
	/* libccid consumes SlotStatus/Parameters with one ReadPort call and does
	 * not iterate their time-extension replies.  Its DataBlock paths do. */
	extend=command==PC_TO_RDR_POWER_ON || command==PC_TO_RDR_XFR_BLOCK;
	if(extend)
		ccid_extension_begin(c,ccid_response_type(command),slot,seq,
			generation,backend_generation,epoch);

	mutex_lock(&c->backend_lock);
    spin_lock_irqsave(&c->state_lock,flags);
	if(!c->online || generation!=c->generation || epoch!=c->command_work_epoch) {
		if(c->extension_epoch==epoch)c->extension_active=false;
		if(c->work_pending && epoch==c->command_work_epoch)c->work_pending=false;
		release=c->online && !c->work_pending;
		spin_unlock_irqrestore(&c->state_lock,flags);
		mutex_unlock(&c->backend_lock);
		if(release)ccid_submit_out(c,GFP_ATOMIC);
		return;
	}
    spin_unlock_irqrestore(&c->state_lock,flags);
    if (c->parameters_generation != generation) c->parameters_valid = false;
    if (backend_generation != c->backend_generation) {
        ccid_reply(response, ccid_response_type(command), slot, seq,
                   MB86H_CARD_ABSENT | CCID_STATUS_FAILED,
                   CCID_ERROR_HW_ERROR, 0, NULL, 0);
        goto submit;
    }
	if (slot != 0) {
		ccid_reply(response, ccid_response_type(command), slot, seq,
			   MB86H_CARD_ABSENT | CCID_STATUS_FAILED,
			   CCID_ERROR_BAD_SLOT, 0, NULL, 0);
		goto submit;
	}

	if (command == PC_TO_RDR_ABORT) {
		int token = 0x10000 | (slot << 8) | seq;
		/* Requests are strictly serialized.  A previous transfer has completed
		 * before the matching bulk Abort is accepted, so acknowledge it without
		 * replaying it.  The class-control half was accepted in ccid_setup(). */
		if (length || frame[7] || !ccid_reserved_zero(frame) ||
		    atomic_cmpxchg(&c->abort_token, token, 0) != token) {
			ccid_reply(response, RDR_TO_PC_SLOTSTATUS, slot, seq,
				   MB86H_CARD_ABSENT | CCID_STATUS_FAILED,
				   CCID_ERROR_CMD_ABORTED, 0, NULL, 0);
			goto submit;
		}
		icc = ccid_status_locked(c, &rc);
		ccid_reply(response, RDR_TO_PC_SLOTSTATUS, slot, seq,
			   icc | (rc ? CCID_STATUS_FAILED : CCID_STATUS_OK),
			   rc ? CCID_ERROR_CMD_ABORTED : 0, 0, NULL, 0);
		goto submit;
	}

    if (atomic_read(&c->abort_token) == (0x10000 | (slot << 8) | seq)) {
        ccid_reply(response, ccid_response_type(command), slot, seq,
                   icc | CCID_STATUS_FAILED, CCID_ERROR_CMD_ABORTED, 0, NULL, 0);
        goto submit;
    }
	icc = ccid_status_locked(c, &rc);
	switch (command) {
	case PC_TO_RDR_GET_SLOT_STATUS:
		if (length || frame[7] || !ccid_reserved_zero(frame)) {
			ccid_reply(response, RDR_TO_PC_SLOTSTATUS, slot, seq,
			   MB86H_CARD_ABSENT | CCID_STATUS_FAILED,
			   CCID_ERROR_BAD_LENGTH, 0, NULL, 0);
			break;
		}
		ccid_reply(response, RDR_TO_PC_SLOTSTATUS, slot, seq,
			   icc | (rc ? CCID_STATUS_FAILED : CCID_STATUS_OK),
			   rc ? CCID_ERROR_HW_ERROR : 0, 0, NULL, 0);
		break;
	case PC_TO_RDR_POWER_ON:
		out_length = sizeof(atr);
		rc = (length || frame[7] || !ccid_reserved_zero(frame) || !c->registered) ?
			-EINVAL : c->ops->power_on(c->opaque, atr, &out_length);
        if(!rc)ccid_cache_icc(c,MB86H_CARD_ACTIVE);
        c->parameters_generation = generation;
        if(!rc && out_length>sizeof(atr))rc=-EPROTO;
        if(!rc)rc=ccid_parameters(c,atr,out_length);
        c->parameters_valid=!rc && c->parameters_length;
        ccid_reply(response,RDR_TO_PC_DATABLOCK,slot,seq,
                   rc?icc|CCID_STATUS_FAILED:MB86H_CARD_ACTIVE,
                   rc?ccid_error_from_errno(rc):0,0,rc?NULL:atr,rc?0:out_length);
		break;
	case PC_TO_RDR_POWER_OFF:
        c->parameters_valid = false;
		rc = (length || frame[7] || !ccid_reserved_zero(frame) || !c->registered) ?
			-EINVAL : c->ops->power_off(c->opaque);
		if(!rc)ccid_cache_icc(c,MB86H_CARD_INACTIVE);
		ccid_reply(response, RDR_TO_PC_SLOTSTATUS, slot, seq,
			   (rc ? icc | CCID_STATUS_FAILED : MB86H_CARD_INACTIVE),
			   rc ? ccid_error_from_errno(rc) : 0, 0, NULL, 0);
		break;
	case PC_TO_RDR_XFR_BLOCK:
		out_length = sizeof(rx);
		rc = (!c->registered || !c->parameters_valid || frame[7] || !ccid_reserved_zero(frame) ||
		      length > MB86H_CARD_MAX_TX) ? -EINVAL :
			c->ops->transfer(c->opaque, frame + CCID_HEADER, length, rx, &out_length);
		if (!rc && out_length <= sizeof(rx))
			ccid_reply(response, RDR_TO_PC_DATABLOCK, slot, seq,
				   MB86H_CARD_ACTIVE, 0, 0, rx, out_length);
		else
			ccid_reply(response, RDR_TO_PC_DATABLOCK, slot, seq,
				   icc | CCID_STATUS_FAILED, ccid_error_from_errno(rc),
				   0, NULL, 0);
		break;
	case PC_TO_RDR_GET_PARAMETERS:
        if (!c->parameters_valid) rc = -EIO;
		if (length || frame[7] || !ccid_reserved_zero(frame)) {
			ccid_reply(response, RDR_TO_PC_PARAMETERS, slot, seq,
				   icc | CCID_STATUS_FAILED, CCID_ERROR_BAD_LENGTH, 0, NULL, 0);
			break;
		}
		ccid_reply(response, RDR_TO_PC_PARAMETERS, slot, seq,
			   icc | (rc ? CCID_STATUS_FAILED : 0), rc ? CCID_ERROR_HW_ERROR : 0,
			   c->protocol, rc ? NULL : c->parameters, rc ? 0 : c->parameters_length);
		break;
	case PC_TO_RDR_SET_PARAMETERS:
	case PC_TO_RDR_RESET_PARAMETERS:
		if (!c->parameters_valid || (command == PC_TO_RDR_SET_PARAMETERS &&
		     (frame[7] != c->protocol || length != c->parameters_length ||
		      !ccid_reserved_zero(frame) ||
		      memcmp(frame + CCID_HEADER, c->parameters, length))) ||
		    (command == PC_TO_RDR_RESET_PARAMETERS &&
		     (length || frame[7] || !ccid_reserved_zero(frame))))
			ccid_reply(response, RDR_TO_PC_PARAMETERS, slot, seq,
				   icc | CCID_STATUS_FAILED, CCID_ERROR_BAD_PROTOCOL, 0, NULL, 0);
		else
			ccid_reply(response, RDR_TO_PC_PARAMETERS, slot, seq, icc, 0,
				   c->protocol, c->parameters, c->parameters_length);
		break;
	default:
		ccid_reply(response, RDR_TO_PC_SLOTSTATUS, slot, seq,
			   icc | CCID_STATUS_FAILED, CCID_ERROR_CMD_NOT_SUPPORTED, 0, NULL, 0);
		break;
	}
submit:
    if (command != PC_TO_RDR_ABORT &&
        atomic_read(&c->abort_token) == (0x10000 | (slot << 8) | seq))
        ccid_reply(response, ccid_response_type(command), slot, seq,
                   icc | CCID_STATUS_FAILED, CCID_ERROR_CMD_ABORTED, 0, NULL, 0);
    (void)ccid_queue_final(c,response,
        CCID_HEADER+ccid_get_le32(response+1),generation,epoch);
    mutex_unlock(&c->backend_lock);
}

static void ccid_in_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct ccid_function *c = req->context;
	unsigned long flags;
	bool queue=false,retry=false,release=false,final_exhausted=false;
	u8 owner;
	(void)ep;
	spin_lock_irqsave(&c->state_lock, flags);
	owner=c->in_owner;c->in_owner=CCID_IN_NONE;
	c->in_flight = false;
	if(owner==CCID_IN_FINAL && ccid_final_current(c)) {
		if(!req->status) {
			c->final_pending=false;c->work_pending=false;c->final_attempts=0;
		} else if(c->final_attempts<CCID_FINAL_MAX_ATTEMPTS)retry=true;
		else final_exhausted=true;
	} else if(owner==CCID_IN_EXTENSION && ccid_final_current(c)) {
		queue=ccid_stage_final_locked(c);
	}
	release=c->online && !c->work_pending && !c->in_flight;
	spin_unlock_irqrestore(&c->state_lock, flags);
	if(queue)schedule_work(&c->io_work);
	if(retry)
		mod_delayed_work(system_wq,&c->extension_work,
			msecs_to_jiffies(CCID_FINAL_RETRY_MS));
	if(final_exhausted)
		pr_err_ratelimited("mb86H_ccid: final IN completion failed after %u attempts; command fenced\n",
			CCID_FINAL_MAX_ATTEMPTS);
	if(release)ccid_submit_out(c,GFP_ATOMIC);
}

static void ccid_out_complete(struct usb_ep *ep, struct usb_request *req)
{
    struct ccid_function *c = req->context;
    unsigned long flags;
    u8 response[CCID_HEADER];
    u32 length, generation,epoch;
    size_t n;
    bool overflow;
    (void)ep;
    spin_lock_irqsave(&c->state_lock, flags);
    c->out_queued = false;
    generation = c->generation;
    if (!c->online || req->status) goto unlock;
    if (!req->actual) goto again;
    overflow = req->actual > sizeof(c->frame) - c->frame_len;
    n = min_t(size_t, req->actual, sizeof(c->frame) - c->frame_len);
    memcpy(c->frame + c->frame_len, req->buf, n);
    c->frame_len += n;
    if (c->frame_len < CCID_HEADER) goto again;
    length = ccid_get_le32(c->frame + 1);
    if (overflow || length > MB86H_CARD_MAX_TX) goto bad;
    c->expected_len = CCID_HEADER + length;
    if (c->frame_len < c->expected_len) goto again;
    if (c->frame_len != c->expected_len) goto bad;
    memcpy(c->command, c->frame, c->expected_len);
    c->command_generation = generation;
    c->command_backend_generation = c->backend_generation;
    if(++c->command_epoch==0)++c->command_epoch;
    c->command_work_epoch=c->command_epoch;
    c->work_pending = true;
    c->frame_len = c->expected_len = 0;
    schedule_work(&c->work);
    goto unlock;
bad:
    ccid_reply(response, ccid_response_type(c->frame[0]), c->frame[5], c->frame[6],
               MB86H_CARD_ABSENT | CCID_STATUS_FAILED,
               CCID_ERROR_BAD_LENGTH, 0, NULL, 0);
    c->frame_len = c->expected_len = 0;
    if(++c->command_epoch==0)++c->command_epoch;
    epoch=c->command_work_epoch=c->command_epoch;
    c->work_pending=true;
    spin_unlock_irqrestore(&c->state_lock, flags);
    (void)ccid_queue_final(c,response,sizeof(response),generation,epoch);
    return;
again:
    spin_unlock_irqrestore(&c->state_lock, flags);
    ccid_submit_out(c, GFP_ATOMIC);
    return;
unlock:
    spin_unlock_irqrestore(&c->state_lock, flags);
}

static void ccid_intr_complete(struct usb_ep *ep, struct usb_request *req)
{
    struct ccid_function *c = req->context;
    unsigned long flags;
    bool pending;
    (void)ep;
    spin_lock_irqsave(&c->state_lock, flags);
    c->intr_queued = false;
    pending = c->slot_change_pending;
    spin_unlock_irqrestore(&c->state_lock, flags);
    if (pending && !req->status) ccid_notify_slot_change(c, GFP_ATOMIC);
}

/* CCID Abort is a two-step operation: the control request names slot and
 * sequence, then the matching bulk PC_to_RDR_Abort receives SlotStatus.  We
 * cannot cancel a physical FW transfer, so this serial frontend waits for it
 * to complete and never replays it. */
static int ccid_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct ccid_function *c = func_to_ccid(f);
	u16 value = le16_to_cpu(ctrl->wValue);

	if (ctrl->bRequestType != (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) ||
	    ctrl->bRequest != 1 || le16_to_cpu(ctrl->wLength))
		return -EOPNOTSUPP;
	if (le16_to_cpu(ctrl->wIndex) != c->interface_id)
		return -EINVAL;
	if ((value & 0xff) != 0)
		return -EINVAL;
	atomic_set(&c->abort_token, 0x10000 | ((value & 0xff) << 8) | (value >> 8));
    f->config->cdev->req->length = 0;
    f->config->cdev->req->zero = 0;
    return usb_ep_queue(f->config->cdev->gadget->ep0,
                        f->config->cdev->req, GFP_ATOMIC);
}

static struct usb_interface_descriptor ccid_intf_desc = {
	.bLength = sizeof(ccid_intf_desc), .bDescriptorType = USB_DT_INTERFACE,
	.bNumEndpoints = 3, .bInterfaceClass = USB_CLASS_CSCID,
	.bInterfaceSubClass = 0, .bInterfaceProtocol = 0,
};
static struct usb_endpoint_descriptor ccid_fs_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN, .bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(64),
};
static struct usb_endpoint_descriptor ccid_fs_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(64),
};
static struct usb_endpoint_descriptor ccid_fs_intr_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN, .bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(2), .bInterval = 16,
};
static struct usb_endpoint_descriptor ccid_hs_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN, .bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};
static struct usb_endpoint_descriptor ccid_hs_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};
static struct usb_endpoint_descriptor ccid_hs_intr_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN, .bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(2), .bInterval = 9,
};
struct ccid_class_descriptor {
	u8 bLength, bDescriptorType; __le16 bcdCCID; u8 bMaxSlotIndex, bVoltageSupport;
	__le32 dwProtocols, dwDefaultClock, dwMaximumClock; u8 bNumClockSupported;
	__le32 dwDataRate, dwMaxDataRate; u8 bNumDataRatesSupported;
	__le32 dwMaxIFSD, dwSynchProtocols, dwMechanical, dwFeatures, dwMaxCCIDMessageLength;
	u8 bClassGetResponse, bClassEnvelope; __le16 wLcdLayout; u8 bPINSupport, bMaxCCIDBusySlots;
} __packed;
static struct ccid_class_descriptor ccid_desc = {
	.bLength = sizeof(ccid_desc), .bDescriptorType = 0x21, .bcdCCID = cpu_to_le16(0x0110),
	/* Voltage is selected only by the FW's automatic selector (zero); no
	 * physical 5 V/3 V/1.8 V capability is claimed by this virtual reader. */
	.bMaxSlotIndex = 0, .bVoltageSupport = 0, .dwProtocols = cpu_to_le32(3),
    /* Virtual 4 MHz bookkeeping: default F372/D1 through F372/D64.
     * FW owns physical clock/rate; no host clock programming is implied. */
	.dwDefaultClock = cpu_to_le32(4000), .dwMaximumClock = cpu_to_le32(4000),
	.dwDataRate = cpu_to_le32(10753), .dwMaxDataRate = cpu_to_le32(688172),
	.dwMaxIFSD = cpu_to_le32(32), .dwFeatures = cpu_to_le32(0x0002043a),
	.dwMaxCCIDMessageLength = cpu_to_le32(CCID_IN_BYTES), .bMaxCCIDBusySlots = 1,
};
static void ccid_disable(struct usb_function *f);

static int ccid_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct ccid_function *c = func_to_ccid(f);
	int rc;
    unsigned long flags;
	if (intf != c->interface_id || alt) return -EINVAL;
    if (READ_ONCE(c->online)) ccid_disable(f);
	rc = config_ep_by_speed(f->config->cdev->gadget, f, c->in_ep); if (rc) return rc;
	rc = usb_ep_enable(c->in_ep); if (rc) return rc;
	rc = config_ep_by_speed(f->config->cdev->gadget, f, c->out_ep); if (rc) goto disable_in;
	rc = usb_ep_enable(c->out_ep); if (rc) goto disable_in;
	rc = config_ep_by_speed(f->config->cdev->gadget, f, c->intr_ep); if (rc) goto disable_out;
	rc = usb_ep_enable(c->intr_ep); if (rc) goto disable_out;
    spin_lock_irqsave(&c->state_lock, flags);
    c->online = true;
    spin_unlock_irqrestore(&c->state_lock, flags);
	/* Slot 0 initially absent/changed; the next status command reports the
	 * registered backend's real state. */
	ccid_notify_slot_change(c, GFP_ATOMIC);
	if (READ_ONCE(c->registered))
		schedule_delayed_work(&c->slot_work, 0);
	ccid_submit_out(c, GFP_ATOMIC);
	return 0;
disable_out: usb_ep_disable(c->out_ep);
disable_in: usb_ep_disable(c->in_ep);
	return rc;
}

static void ccid_disable(struct usb_function *f)
{
	struct ccid_function *c = func_to_ccid(f);
	unsigned long flags;
    spin_lock_irqsave(&c->submit_lock, flags);
    spin_lock(&c->state_lock);
    c->online = false; ++c->generation;
    c->frame_len = c->expected_len = 0;
    c->in_pending = false;
    c->extension_active=false;c->final_pending=false;
    c->in_owner=CCID_IN_NONE;c->final_attempts=0;
    c->in_flight = false;
    atomic_set(&c->abort_token, 0);
    spin_unlock(&c->state_lock);
    /* Queue and disable are serialized; inline completions use state_lock. */
	usb_ep_disable(c->in_ep); usb_ep_disable(c->out_ep); usb_ep_disable(c->intr_ep);
    spin_unlock_irqrestore(&c->submit_lock, flags);
    cancel_delayed_work(&c->slot_work);
	cancel_delayed_work(&c->extension_work);
	/* This callback must not wait.  ccid_unbind synchronizes work before it
	 * frees endpoint requests, and the worker sees online=false. */
}

static int ccid_bind(struct usb_configuration *cfg, struct usb_function *f)
{
	struct ccid_function *c = func_to_ccid(f); int id;
    struct usb_interface_descriptor intf = ccid_intf_desc;
    struct usb_endpoint_descriptor fs_in = ccid_fs_in_desc, fs_out = ccid_fs_out_desc,
        fs_intr = ccid_fs_intr_desc, hs_in = ccid_hs_in_desc,
        hs_out = ccid_hs_out_desc, hs_intr = ccid_hs_intr_desc;
    struct usb_descriptor_header *fs[] = {
        (void *)&intf, (void *)&ccid_desc, (void *)&fs_out, (void *)&fs_in, (void *)&fs_intr, NULL,
    };
    struct usb_descriptor_header *hs[] = {
        (void *)&intf, (void *)&ccid_desc, (void *)&hs_out, (void *)&hs_in, (void *)&hs_intr, NULL,
    };
	id = usb_interface_id(cfg, f); if (id < 0) return id;
	intf.bInterfaceNumber = id;
	c->interface_id = id;
    intf.iInterface = container_of(c, struct ccid_reader, ccid)->strings[USB_GADGET_PRODUCT_IDX].id;
	c->in_ep = usb_ep_autoconfig(cfg->cdev->gadget, &fs_in);
	c->out_ep = usb_ep_autoconfig(cfg->cdev->gadget, &fs_out);
	c->intr_ep = usb_ep_autoconfig(cfg->cdev->gadget, &fs_intr);
	if (!c->in_ep || !c->out_ep || !c->intr_ep) return -ENODEV;
	hs_in.bEndpointAddress = fs_in.bEndpointAddress;
	hs_out.bEndpointAddress = fs_out.bEndpointAddress;
	hs_intr.bEndpointAddress = fs_intr.bEndpointAddress;
	c->in_req = usb_ep_alloc_request(c->in_ep, GFP_KERNEL);
	c->out_req = usb_ep_alloc_request(c->out_ep, GFP_KERNEL);
	c->intr_req = usb_ep_alloc_request(c->intr_ep, GFP_KERNEL);
	if (!c->in_req || !c->out_req || !c->intr_req) goto fail;
	c->in_req->buf = kzalloc(CCID_IN_BYTES, GFP_KERNEL);
	c->out_req->buf = kzalloc(CCID_OUT_PACKET, GFP_KERNEL);
	c->intr_req->buf = kzalloc(2, GFP_KERNEL);
	if (!c->in_req->buf || !c->out_req->buf || !c->intr_req->buf) goto fail;
	c->in_req->context = c; c->in_req->complete = ccid_in_complete;
	c->out_req->context = c; c->out_req->complete = ccid_out_complete;
	c->intr_req->context = c; c->intr_req->complete = ccid_intr_complete;
	f->fs_descriptors = usb_copy_descriptors(fs);
	f->hs_descriptors = usb_copy_descriptors(hs);
	if (f->fs_descriptors && f->hs_descriptors)
		return 0;
fail:
	usb_free_all_descriptors(f);
	if (c->in_req) { kfree(c->in_req->buf); usb_ep_free_request(c->in_ep, c->in_req); c->in_req = NULL; }
	if (c->out_req) { kfree(c->out_req->buf); usb_ep_free_request(c->out_ep, c->out_req); c->out_req = NULL; }
	if (c->intr_req) { kfree(c->intr_req->buf); usb_ep_free_request(c->intr_ep, c->intr_req); c->intr_req = NULL; }
	return -ENOMEM;
}

static void ccid_unbind(struct usb_configuration *cfg, struct usb_function *f)
{
	struct ccid_function *c = func_to_ccid(f); (void)cfg;
	/* usb_composite invokes unbind in teardown process context, after disable.
	 * This is the lifetime fence for the endpoint buffers and backend calls. */
	cancel_work_sync(&c->work);
	cancel_delayed_work_sync(&c->slot_work);
	cancel_delayed_work_sync(&c->extension_work);
	cancel_work_sync(&c->io_work);
	usb_free_all_descriptors(f);
	if (c->in_req) { kfree(c->in_req->buf); usb_ep_free_request(c->in_ep, c->in_req); c->in_req = NULL; }
	if (c->out_req) { kfree(c->out_req->buf); usb_ep_free_request(c->out_ep, c->out_req); c->out_req = NULL; }
	if (c->intr_req) { kfree(c->intr_req->buf); usb_ep_free_request(c->intr_ep, c->intr_req); c->intr_req = NULL; }
}

static int ccid_config_bind(struct usb_configuration *cfg)
{
    struct ccid_reader *r = container_of(cfg, struct ccid_reader, config);
    struct usb_function *f = &r->ccid.function;
    f->name = "mb86H_ccid"; f->bind = ccid_bind; f->unbind = ccid_unbind;
    f->set_alt = ccid_set_alt; f->disable = ccid_disable; f->setup = ccid_setup;
    return usb_add_function(cfg, f);
}

/* All strings/descriptors belong to this physical reader until unregister
 * drains composite unbind, including failed-bind cleanup. */
static int ccid_composite_bind(struct usb_composite_dev *cdev)
{
    struct ccid_reader *r = container_of(cdev->driver, struct ccid_reader, driver);
    int rc;
    /* usb_composite_probe resets gadget_driver fields in Linux 6.18. Enforce
     * the selected dummy controller at bind, before adding any function. */
    if (!cdev->gadget->dev.parent ||
        strcmp(dev_name(cdev->gadget->dev.parent), r->udc_name)) return -ENODEV;
    rc = usb_string_ids_tab(cdev, r->strings);
    if (rc < 0) return rc;
    r->device.iManufacturer = r->strings[USB_GADGET_MANUFACTURER_IDX].id;
    r->device.iProduct = r->strings[USB_GADGET_PRODUCT_IDX].id;
    r->device.iSerialNumber = r->strings[USB_GADGET_SERIAL_IDX].id;
    return usb_add_config(cdev, &r->config, ccid_config_bind);
}

static void ccid_reader_free(struct ccid_reader *r)
{
    ida_free(&ccid_ids, r->id);
    kfree(r->product); kfree(r->serial); kfree(r);
}

int mb86H_ccid_register(const struct mb86H_ccid_ops *ops, void *opaque,
                        const char *product, const char *serial)
{
    struct ccid_reader *r, *other;
    struct ccid_function *c;
    unsigned first;
    char tail;
    int rc;
    if (!ops || !opaque || !product || !serial || !ops->status ||
        !ops->power_on || !ops->power_off || !ops->transfer) return -EINVAL;
    if (sscanf(ccid_udc, "dummy_udc.%u%c", &first, &tail) != 1) return -EINVAL;
    mutex_lock(&ccid_lifecycle_lock);
    list_for_each_entry(other, &ccid_readers, node) {
        if (other->ccid.opaque == opaque) { rc = -EALREADY; goto out; }
    }
    r = kzalloc(sizeof(*r), GFP_KERNEL);
    if (!r) { rc = -ENOMEM; goto out; }
    r->id = ida_alloc(&ccid_ids, GFP_KERNEL);
    if (r->id < 0) { rc = r->id; kfree(r); goto out; }
    r->product = kstrdup(product, GFP_KERNEL); r->serial = kstrdup(serial, GFP_KERNEL);
    if (!r->product || !r->serial) { rc = -ENOMEM; goto free_reader; }
    snprintf(r->driver_name, sizeof(r->driver_name), "mb86H_ccid.%d", r->id);
    snprintf(r->udc_name, sizeof(r->udc_name), "dummy_udc.%u", first + r->id);
    r->strings[USB_GADGET_MANUFACTURER_IDX].s = "mb86H project";
    r->strings[USB_GADGET_PRODUCT_IDX].s = r->product;
    r->strings[USB_GADGET_SERIAL_IDX].s = r->serial;
    r->language.language = 0x0409; r->language.strings = r->strings;
    r->string_tables[0] = &r->language;
    r->device = (struct usb_device_descriptor) {
        .bLength = sizeof(r->device), .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = cpu_to_le16(0x0200), .idVendor = cpu_to_le16(CCID_VID),
        .idProduct = cpu_to_le16(CCID_PID), .bNumConfigurations = 1,
    };
    r->config.label = "CCID"; r->config.bConfigurationValue = 1;
    r->config.bmAttributes = USB_CONFIG_ATT_SELFPOWER; r->config.MaxPower = 1;
    r->driver.name = r->driver_name; r->driver.dev = &r->device;
    r->driver.strings = r->string_tables; r->driver.max_speed = USB_SPEED_HIGH;
    r->driver.bind = ccid_composite_bind;
    c = &r->ccid;
    mutex_init(&c->backend_lock); spin_lock_init(&c->state_lock); spin_lock_init(&c->submit_lock);
    INIT_WORK(&c->work, ccid_work); INIT_DELAYED_WORK(&c->slot_work, ccid_slot_work);
	INIT_DELAYED_WORK(&c->extension_work, ccid_extension_work);
    INIT_WORK(&c->io_work, ccid_io_work);
    c->slot_change_value = 0x02;
    c->ops = ops; c->opaque = opaque; c->registered = true;
    ccid_backend_changed(c);
    rc = usb_composite_probe(&r->driver);
    if (rc) goto free_reader;
    if (!c->function.config) {
        usb_composite_unregister(&r->driver);
        rc = -ENODEV;
        goto free_reader;
    }
    list_add_tail(&r->node, &ccid_readers);
    schedule_delayed_work(&c->slot_work, 0);
    pr_info("%s: reader %s serial=%s UDC=%s\n", r->driver_name, product, serial, r->udc_name);
    rc = 0;
    goto out;
free_reader:
    ccid_reader_free(r);
out:
    mutex_unlock(&ccid_lifecycle_lock);
    return rc;
}
EXPORT_SYMBOL_GPL(mb86H_ccid_register);

void mb86H_ccid_unregister(void *opaque)
{
    struct ccid_reader *r, *next;
    if (!opaque) return;
    mutex_lock(&ccid_lifecycle_lock);
    list_for_each_entry_safe(r, next, &ccid_readers, node) {
        if (r->ccid.opaque != opaque) continue;
        mutex_lock(&r->ccid.backend_lock);
        r->ccid.registered = false; r->ccid.ops = NULL; r->ccid.opaque = NULL;
        ccid_backend_changed(&r->ccid);
        mutex_unlock(&r->ccid.backend_lock);
        usb_composite_unregister(&r->driver);
        list_del(&r->node);
        ccid_reader_free(r);
        break;
    }
    mutex_unlock(&ccid_lifecycle_lock);
}
EXPORT_SYMBOL_GPL(mb86H_ccid_unregister);

static int __init mb86H_ccid_module_init(void)
{
    unsigned first;
    char tail;
    return ccid_udc && sscanf(ccid_udc,"dummy_udc.%u%c",&first,&tail)==1 ? 0 : -EINVAL;
}
static void __exit mb86H_ccid_module_exit(void)
{
    WARN_ON(!list_empty(&ccid_readers));
    ida_destroy(&ccid_ids);
}
module_init(mb86H_ccid_module_init);
module_exit(mb86H_ccid_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("mb86H Series USB CCID smart card reader driver");
