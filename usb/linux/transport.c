#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "../../linux/dvb.h"
#include "../../linux/ccid.h"
#include "mb86H/card.h"
#include "mb86H/protocol.h"
#include "mb86H/device.h"
#include "mb86H/usb/boot.h"
#include "mb86H/usb/window.h"
#include "mb86H/loader.h"
#include "mb86H/session.h"
#include "mb86H/device_query.h"
#include "mb86H/protocol/constants.h"

/* Defined in firmware_embedded.S; bundle supplied as firmware/firmware.bin. */
extern const u8 firmware_embedded_data[];
extern const u8 firmware_embedded_end[];
#define USB_WINDOW_REQUEST 0xbc
#define USB_WINDOW_TIMEOUT_MS 700
#define USB_STREAM_TIMEOUT_MS 100
#define USB_STREAM_BUFFER_BYTES 16384
#define USB_RX_QUEUE_LIMIT 64u
#define USB_RX_URBS 4u

struct usb_rx_packet {
	struct list_head node;
	size_t length;
	u64 queued_ns;
	u8 data[];
};

/* Payload-free, per-epoch observations. Completion fields use rx_lock;
 * worker fields have one writer, wire fields use control_lock. Stop reads
 * them only after all URBs and receive work have drained. */
struct usb_rx_stats {
    u64 started_ns, first_ns, last_ns, bytes, completions;
    u64 completion_gap_ns, queue_wait_ns, delivery_ns;
    u64 delivered_bytes, dropped_packets, allocation_failures, overflows;
    u64 service_last_ns, service_gap_ns, service_duration_ns, service_calls;
    u64 steady_service_last_ns, steady_service_gap_ns, steady_service_duration_ns;
    u64 steady_service_calls, steady_service_gaps_10ms, steady_service_gaps_40ms;
    u64 steady_service_total_ns;
    size_t queued_bytes, queue_high_bytes;
    unsigned int queue_high_packets;
    bool active;
};

struct usb_rx_transfer {
	struct urb *urb;
	u8 *buffer;
	dma_addr_t dma;
};

struct usb_host {
    const struct mb86H_device_info *device;
    struct mb86H_dvb_caps caps[MB86H_CHANNEL_COUNT];
    bool torne_block_writes;
    struct usb_xz_read_cache xz_read_cache;
	struct usb_device *udev;
	struct usb_interface *intf;
	struct mb86H_dvb *dvb;
	struct mutex control_lock;
	spinlock_t rx_lock;
	struct list_head rx_queue;
	struct work_struct rx_work;
	struct delayed_work service_work;
	bool streaming;
	bool service_shutdown;
	bool halt_attempted;
    bool tune_polling;
    bool suspended;
    int pm_error, halt_status;
	int stream_error;
	int rx_error;
	unsigned int rx_queued;
	struct usb_rx_stats rx_stats;
	struct usb_rx_transfer rx[USB_RX_URBS];
	bool stopping;
	struct mb86H_loader loader;
	struct mb86H_loader_ops loader_ops;
	struct mb86H_session session;
	struct mb86H_card card;
	bool ccid_registered;
	int control_failed;
    u8 last_response[64];
    struct ida *alias_ids;
    int alias_id;
    bool alias_registered;
    char alias[24];
	u8 stream_mask;
	unsigned int stream_formats[MB86H_CHANNEL_COUNT];
};

static DEFINE_IDA(usb_xz_alias_ids);
static DEFINE_IDA(usb_torne_alias_ids);
static DEFINE_IDA(usb_dt295_alias_ids);

static ssize_t mb86H_alias_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct usb_host *x = usb_get_intfdata(to_usb_interface(dev));
    (void)attr;
    return x ? sysfs_emit(buf, "%s\n", x->alias) : -ENODEV;
}
static DEVICE_ATTR_RO(mb86H_alias);

static void usb_alias_remove(struct usb_host *x)
{
    if (x->alias_registered) {
        device_remove_file(&x->intf->dev, &dev_attr_mb86H_alias);
        x->alias_registered = false;
    }
    if (x->alias_id >= 0) {
        ida_free(x->alias_ids, x->alias_id);
        x->alias_id = -1;
    }
}

static int usb_alias_register(struct usb_host *x)
{
    bool torne = x->device->vendor == USB_TORNE_VENDOR_ID;
	bool dt295 = x->device->firmware_target == MB86H_FIRMWARE_DT295;
	const char *prefix = dt295 ? "dt295" : torne ? "torne" : "xz";
    int rc;
	x->alias_ids = dt295 ? &usb_dt295_alias_ids :
	    torne ? &usb_torne_alias_ids : &usb_xz_alias_ids;
    x->alias_id = ida_alloc(x->alias_ids, GFP_KERNEL);
    if (x->alias_id < 0) return x->alias_id;
	snprintf(x->alias, sizeof(x->alias), "%s%d", prefix, x->alias_id);
    rc = device_create_file(&x->intf->dev, &dev_attr_mb86H_alias);
    if (rc) { usb_alias_remove(x); return rc; }
    x->alias_registered = true;
    return 0;
}

static void usb_stop_rx(struct usb_host *x);
static int usb_start_rx(struct usb_host *x);

static void *usb_loader_allocate(void *opaque, size_t size)
{
	(void)opaque;
	return kvmalloc(size, GFP_KERNEL);
}

static void usb_loader_release(void *opaque, void *memory)
{
	(void)opaque;
	kvfree(memory);
}

static uint32_t usb_loader_now_ms(void *opaque)
{
	(void)opaque;
	return (uint32_t)jiffies_to_msecs(jiffies);
}

static void usb_loader_delay_ms(void *opaque, unsigned int milliseconds)
{
	(void)opaque;
	msleep(milliseconds);
}

/* DVB's control mutex retains RF admission across this wait. Only the wire
 * mutex is yielded, so card packets can progress between RF polling turns. */
static void usb_session_delay_ms(void *opaque, unsigned int milliseconds)
{
    struct usb_host *x=opaque;
    bool yielding=x->tune_polling;
    if(yielding)mutex_unlock(&x->control_lock);
    msleep(milliseconds);
    if(yielding)mutex_lock(&x->control_lock);
}

/* The bus window is intentionally bounded; the common loader owns firmware addresses. */

/* The request is the already validated boot0 property window used by the
 * retained GV-MVP/XZ loader.  No generic register or I2C ABI is exposed here. */
static int usb_window_locked(struct usb_host *x, u8 direction, u32 address,
				     void *data, u16 length, unsigned int timeout)
{
	int actual;
	u8 *buffer;
	x->xz_read_cache.valid = false;

	if (!length || !data || address > 0xfffffu ||
	    address + length > 0x100000u)
		return -EINVAL;
	buffer = kmalloc(length, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	if (!(direction & USB_DIR_IN))
		memcpy(buffer, data, length);
	actual = usb_control_msg(x->udev,
		(direction & USB_DIR_IN) ? usb_rcvctrlpipe(x->udev, 0) :
		usb_sndctrlpipe(x->udev, 0), USB_WINDOW_REQUEST, direction,
		(u16)(((address >> 16) & 0xfu) << 8), (u16)address, buffer,
		length, timeout);
	if ((direction & USB_DIR_IN) && actual == length)
		memcpy(data, buffer, length);
	kfree(buffer);
	if (actual != length)
		dev_err(&x->intf->dev,
			"boot0 control failed direction=%s address=%08x length=%u timeout_ms=%u actual=%d seq=%u\n",
			(direction & USB_DIR_IN) ? "IN" : "OUT", address,
			length, timeout, actual, x->session.sequence);
	if (actual < 0)
		return actual;
	return actual == length ? 0 : -EIO;
}

static int usb_window_read_locked(struct usb_host *x, u32 address,
					  void *data, u16 length)
{
	return usb_window_locked(x, USB_DIR_IN | USB_TYPE_VENDOR |
				 USB_RECIP_DEVICE, address, data, length,
				 address == MB86H_BOOT0_STAGING_ADDRESS + 0 ? 20000 :
				 USB_WINDOW_TIMEOUT_MS);
}

static int usb_window_write_locked(struct usb_host *x, u32 address,
					   const void *data, u16 length)
{
	return usb_window_locked(x, USB_DIR_OUT | USB_TYPE_VENDOR |
				 USB_RECIP_DEVICE, address, (void *)data, length,
				 USB_WINDOW_TIMEOUT_MS);
}

static int usb_native_control(void *opaque, const struct usb_window_control *control,
                               void *data, unsigned timeout)
{
    struct usb_host *x = opaque;
    x->xz_read_cache.valid = false;
    bool reading = control->request_type & USB_DIR_IN;
    u8 *buffer = kmalloc(control->length, GFP_KERNEL);
    int actual;
    if (!buffer) return -ENOMEM;
    if (!reading) memcpy(buffer, data, control->length);
    actual = usb_control_msg(x->udev, reading ? usb_rcvctrlpipe(x->udev, 0) :
        usb_sndctrlpipe(x->udev, 0), control->request, control->request_type,
        control->value, control->index, buffer, control->length, timeout);
    if (reading && actual == control->length) memcpy(data, buffer, control->length);
    kfree(buffer);
    if (actual != control->length) {
        x->control_failed = actual < 0 ? actual : -EIO;
        return x->control_failed;
    }
    return 0;
}
static const struct usb_native_ops usb_native_ops = {.control = usb_native_control};

/* Preserve XZ's established per-control deadlines and error reporting. */
static int usb_xz_control(void *opaque, const struct usb_window_control *control,
                          void *data, unsigned timeout)
{
    struct usb_host *x = opaque;
    u32 address = ((u32)(control->value & 0x0f00u) << 8) | control->index;
    bool reading = control->request_type & USB_DIR_IN;
    (void)timeout;
    return usb_window_locked(x, control->request_type, address, data,
        control->length, reading && address == MB86H_BOOT0_STAGING_ADDRESS ?
        20000 : USB_WINDOW_TIMEOUT_MS);
}
static const struct usb_native_ops usb_xz_ops = {.control = usb_xz_control};

static const struct usb_native_window_profile *usb_native_window_profile(
    const struct usb_host *x)
{
    if (x->device && x->device->usb_boot && x->device->usb_boot->window)
        return x->device->usb_boot->window;
    /* Torne predates the registry-owned boot descriptor. Keep its established
     * native profile while DT295 selects its profile through usb_boot above. */
    return x->loader.is_torne ? &usb_torne_window_profile : NULL;
}

static int usb_loader_block_writes(void *opaque, bool enable)
{
    struct usb_host *x = opaque;
    if (!usb_native_window_profile(x)) return -EINVAL;
    x->torne_block_writes = enable;
    return 0;
}

/* Raw packet framing is owned by core/session.c. */

static int usb_session_exchange(void *opaque, const uint8_t request[64],
					uint8_t response[64])
{
	struct usb_host *x = opaque;
	const struct usb_native_window_profile *profile;
	u64 service_begin = 0;
	int rc;
	if (x->control_failed)
		return x->control_failed;
	if (x->rx_stats.active &&
	    mb86H_debug_u32be_get(request + 8) == MB86H_DEBUG_RESIDENT_DISPATCH &&
	    mb86H_debug_u32be_get(request + 12) == MB86H_RESIDENT_COMMAND_SERVICE)
		service_begin = ktime_get_ns();
	profile = usb_native_window_profile(x);
	if (profile) {
		rc = usb_native_exchange(&usb_native_ops, x, profile, request, response,
					   x->torne_block_writes);
	} else {
		rc = usb_xz_exchange_cached(&usb_xz_ops, x, &x->xz_read_cache,
					    request, response);
	}
	if (service_begin) {
		struct usb_rx_stats *s = &x->rx_stats;
		u64 elapsed = ktime_get_ns() - service_begin;
		if (s->service_last_ns)
			s->service_gap_ns = max(s->service_gap_ns, service_begin - s->service_last_ns);
		s->service_last_ns = service_begin;
		s->service_duration_ns = max(s->service_duration_ns, elapsed);
		++s->service_calls;
		if (x->streaming) {
			if (s->steady_service_last_ns) {
				u64 gap = service_begin - s->steady_service_last_ns;
				s->steady_service_gap_ns = max(s->steady_service_gap_ns, gap);
				if (gap > 10000000u) ++s->steady_service_gaps_10ms;
				if (gap > 40000000u) ++s->steady_service_gaps_40ms;
			}
			s->steady_service_last_ns = service_begin;
			s->steady_service_duration_ns = max(s->steady_service_duration_ns, elapsed);
			s->steady_service_total_ns += elapsed;
			++s->steady_service_calls;
		}
	}
	return rc;
}

static int usb_loader_window_read(void *opaque, uint32_t address,
                                  void *data, uint16_t length)
{
    struct usb_host *x = opaque;
    const struct usb_native_window_profile *profile = usb_native_window_profile(x);
    return profile ? usb_native_window(&usb_native_ops, x, profile, address, data,
                                       length, false, false) :
        usb_window_read_locked(x, address, data, length);
}

static int usb_loader_window_write(void *opaque, uint32_t address,
                                   const void *data, uint16_t length)
{
    struct usb_host *x = opaque;
    const struct usb_native_window_profile *profile = usb_native_window_profile(x);
    return profile ? usb_native_window(&usb_native_ops, x, profile, address,
                                       (void *)data, length, true,
                                       x->torne_block_writes) :
        usb_window_write_locked(x, address, data, length);
}

static void usb_session_observe(void *opaque, const uint8_t request[64],
                                 const uint8_t *response, int result)
{
    struct usb_host *x = opaque;
    /* Factory boot0 echoes the side-effect-free gateway probe. The classifier
     * consumes this expected response; it is not a failed resident command. */
    if (!result && response &&
        mb86H_debug_u32be_get(request + 8) == MB86H_DEBUG_PING &&
        !memcmp(request, response, 64)) return;
    if (mb86H_debug_u32be_get(request + 12) == MB86H_RESIDENT_COMMAND_CARD) {
        /* Card application bytes never belong in the kernel log. */
        if (result || (response && mb86H_debug_u32be_get(response + 8) != MB86H_DEBUG_OK))
            dev_err_ratelimited(&x->intf->dev,
                "card command=%u seq=%u transport=%d firmware=%d\n",
                mb86H_debug_u32be_get(request + 16),
                mb86H_debug_u32be_get(request + 4), result,
                response ? (s32)mb86H_debug_u32be_get(response + 20) : 0);
        return;
    }
    if (response) memcpy(x->last_response, response, sizeof(x->last_response));
    if (result || (response &&
        mb86H_debug_u32be_get(response + 8) != MB86H_DEBUG_OK))
        dev_err(&x->intf->dev,
                "FW exchange failed seq=%u op=%u command=%x result=%d response=%*phN\n",
                mb86H_debug_u32be_get(request + 4),
                mb86H_debug_u32be_get(request + 8),
                mb86H_debug_u32be_get(request + 12), result,
                response ? 64 : 0, response);
    else if (response && mb86H_debug_u32be_get(request + 12) ==
             MB86H_RESIDENT_COMMAND_SOURCE_STATUS && request[24] == 2)
        dev_err(&x->intf->dev, "board restore diagnostic response=%*phN\n", 64, response);
}

static const struct mb86H_session_ops usb_session_ops = {
	.exchange = usb_session_exchange,
    .observe = usb_session_observe,
	.now_ms = usb_loader_now_ms,
	.delay_ms = usb_session_delay_ms,
};

static int usb_query_dvb_sources_locked(struct usb_host *x,
                                        const struct mb86H_device_overview *overview)
{
    unsigned source;

    for (source = 0; source < MB86H_CHANNEL_COUNT; ++source) {
        struct mb86H_source_descriptor descriptor;
        struct mb86H_dvb_caps *caps = &x->caps[source];
        unsigned int bit = 1u << source;
        int rc;

        caps->present = 0;
        x->stream_formats[source] = MB86H_STREAM_FORMAT_XZM1;
        if (!(overview->rf_mask & bit))
            continue;
        if (!(x->device->delivery_systems & bit))
            return -EPROTO;
        rc = mb86H_device_query_source(&x->session, &usb_session_ops, x,
                                       source, &descriptor);
        if (rc)
            return rc;
        if (!!(overview->stream_mask & bit) !=
            !!(descriptor.flags & MB86H_DEVICE_QUERY_STREAM))
            return -EPROTO;
        *caps = (struct mb86H_dvb_caps) {
            .frequency_min = descriptor.frequency_min,
            .frequency_max = descriptor.frequency_max,
            .present = 1,
            .record_size = descriptor.record_size,
            .auto_tsid = !!(descriptor.flags & MB86H_DEVICE_QUERY_AUTO_TSID),
            .mux_channel = source,
        };
        x->stream_formats[source] = descriptor.stream_format;
    }
    return mb86H_dvb_caps_valid(x->caps) ? 0 : -EPROTO;
}

static void usb_loader_mismatch(void *opaque, uint32_t address,
                                  uint32_t expected, uint32_t observed)
{
    struct usb_host *x = opaque;
    dev_err(&x->intf->dev, "firmware readback mismatch address=%x expected=%x got=%x\n",
            address, expected, observed);
}

static const struct mb86H_loader_ops usb_loader_ops_template = {
	.window_read = usb_loader_window_read,
    .block_writes = usb_loader_block_writes,
	.window_write = usb_loader_window_write,
	.session = NULL,
	.session_ops = &usb_session_ops,
	.now_ms = usb_loader_now_ms,
	.delay_ms = usb_loader_delay_ms,
	.allocate = usb_loader_allocate,
	.release = usb_loader_release,
    .mismatch = usb_loader_mismatch,
};

/* Each card packet shares sequence numbers and the physical control owner
 * with DVB. The card client releases this lock between timed service steps. */
static int usb_card_command(void *opaque, unsigned operation,
                            const u8 data[40], u8 response[64])
{
    struct usb_host *x = opaque;
    int rc;
    mutex_lock(&x->control_lock);
    if (x->service_shutdown)
        rc = -ESHUTDOWN;
    else {
        rc = mb86H_session_command(&x->session, &usb_session_ops, x,
            MB86H_RESIDENT_COMMAND_CARD, operation, data, response);
        if (rc) x->control_failed = rc;
    }
    mutex_unlock(&x->control_lock);
    return rc;
}

static const struct mb86H_card_ops usb_card_ops = {
    .command = usb_card_command,
    .now_ms = usb_loader_now_ms,
    .delay_ms = usb_loader_delay_ms,
};

static int usb_card_status(void *opaque, u8 *status)
{
    struct usb_host *x = opaque;
    return mb86H_card_status(&x->card, status);
}

static int usb_card_power_on(void *opaque, u8 *atr, size_t *length)
{
    struct usb_host *x = opaque;
    return x->card.active ? mb86H_card_reset(&x->card, atr, length) :
                            mb86H_card_power_on(&x->card, atr, length);
}

static int usb_card_power_off(void *opaque)
{
    struct usb_host *x = opaque;
    return mb86H_card_power_off(&x->card);
}

static int usb_card_transfer(void *opaque, const u8 *tx, size_t tx_length,
                             u8 *rx, size_t *rx_length)
{
    struct usb_host *x = opaque;
    return mb86H_card_transfer(&x->card, tx, tx_length, rx, rx_length);
}

static const struct mb86H_ccid_ops usb_ccid_ops = {
    .status = usb_card_status,
    .power_on = usb_card_power_on,
    .power_off = usb_card_power_off,
    .transfer = usb_card_transfer,
};

/* Session polling and cleanup are owned by core/session.c. */

static int usb_tune(void *opaque, unsigned int source,
			    const struct mb86H_dvb_request *request)
{
	struct usb_host *x = opaque;
	int rc;

	if (!request || source > 1)
		return -EINVAL;
	mutex_lock(&x->control_lock);
	if (x->service_shutdown) {
		mutex_unlock(&x->control_lock);
		return -ESHUTDOWN;
	}
	x->stream_error = 0;
    x->tune_polling=true;
	rc = mb86H_session_tune(&x->session, &usb_session_ops, x, source,
				    request, false);
    x->tune_polling=false;
    if (rc) {
        dev_err(&x->intf->dev, "tune failed=%d last semantic response=%*phN\n",
                    rc, 64, x->last_response);
        if (rc == -ETIMEDOUT && !x->control_failed &&
            mb86H_debug_u32be_get(x->last_response + 4) == x->session.sequence &&
            (s32)mb86H_debug_u32be_get(x->last_response + 36 + source * 4) == -2603) {
            uint8_t data[40] = {MB86H_RESIDENT_SOURCE_LOCK_OBSERVE}, response[64];
            int observed = mb86H_session_command(&x->session, &usb_session_ops, x,
                MB86H_RESIDENT_COMMAND_SOURCE_STATUS, source, data, response);
            dev_err(&x->intf->dev, "cached lock status source=%u transport=%d response=%*phN\n",
                source, observed, observed ? 0 : 64, response);
        }
    }
	mutex_unlock(&x->control_lock);
	return rc;
}

static int usb_stream(void *opaque, unsigned int source, bool enable)
{
	struct usb_host *x = opaque;
	int rc;

	if (source > 1)
		return -EINVAL;
	if (enable && !(READ_ONCE(x->stream_mask) & (1u << source)))
		return -EOPNOTSUPP;
	mutex_lock(&x->control_lock);
    if (x->service_shutdown) {
        mutex_unlock(&x->control_lock);
        return -ESHUTDOWN;
    }
	if (!mb86H_session_matches(&x->session, source)) {
		rc = -EINVAL;
		goto out;
	}
    if(!enable) {
        x->streaming=false;x->stream_error=0;
        mb86H_session_set_streaming(&x->session, false);
        cancel_delayed_work(&x->service_work);
        /* Application RX ends before the single STOP exchange. */
        usb_stop_rx(x);
    }
    if(enable && x->session.stop_pending) {
        rc=mb86H_session_wait_stopped(&x->session,&usb_session_ops,x);
        if(rc)goto out;
    }
    if(enable && !x->rx[0].urb) {
        WRITE_ONCE(x->rx_error,0);
        WRITE_ONCE(x->stopping,false);
        rc=usb_start_rx(x);
        if(rc) { usb_stop_rx(x);goto out; }
    }
    rc = mb86H_session_stream(&x->session, &usb_session_ops, x, source, enable);
	if (rc)
		dev_err(&x->intf->dev,
			"stream source=%u enable=%u failed=%d last semantic response=%*phN\n",
			source, enable, rc, 64, x->last_response);
	if (!rc) {
		x->streaming = enable;
		mb86H_session_set_streaming(&x->session, enable);
        x->stream_error=0;
		if (enable)
			schedule_delayed_work(&x->service_work, 0);
	}
out:
    if(!enable || rc) {
        /* Closing a feed terminates host RX even if hardware stop fails. */
        usb_stop_rx(x);
        if(rc) {
            x->streaming=false;
            x->stream_error=rc;
            mb86H_dvb_stream_error(x->dvb,rc);
            schedule_delayed_work(&x->service_work,0);
        }
    }
	mutex_unlock(&x->control_lock);
	return rc;
}

static int usb_locked(void *opaque, unsigned int source, bool *locked)
{
	struct usb_host *x = opaque;
	int rc;

	if (!locked || source > 1)
		return -EINVAL;
	mutex_lock(&x->control_lock);
	rc = x->service_shutdown ? -ESHUTDOWN :
        mb86H_session_locked(&x->session, &usb_session_ops, x, source, locked);
	mutex_unlock(&x->control_lock);
	return rc;
}

static void usb_halt(void *opaque)
{
	struct usb_host *x = opaque;
	int rc;

	if (READ_ONCE(x->halt_attempted))
		return;
	WRITE_ONCE(x->halt_attempted,true);
    WRITE_ONCE(x->service_shutdown,true);
	WRITE_ONCE(x->streaming, false);
	cancel_delayed_work_sync(&x->service_work);
	mutex_lock(&x->control_lock);
	rc = x->pm_error ? x->pm_error : x->control_failed ? x->control_failed :
	    mb86H_loader_close(&x->loader, &x->loader_ops, x);
	x->halt_status = rc;
	mutex_unlock(&x->control_lock);
	usb_stop_rx(x);
	dev_info(&x->intf->dev, "firmware close status=%d\n", rc);
}

static const struct mb86H_dvb_ops usb_dvb_ops = {
	.tune = usb_tune,
	.stream = usb_stream,
	.locked = usb_locked,
	.halt = usb_halt,
};


static void usb_rx_failed(struct usb_host *x,int error)
{
    if(error>=0 || READ_ONCE(x->stopping))return;
    if(cmpxchg(&x->rx_error,0,error)==0)
        dev_err_ratelimited(&x->intf->dev,"EP81 receive failed: %d\n",error);
    schedule_delayed_work(&x->service_work,0);
}

static void usb_service_work(struct work_struct *work)
{
    struct usb_host *x=container_of(to_delayed_work(work),struct usb_host,service_work);
    int rc=0;
    mutex_lock(&x->control_lock);
    if(READ_ONCE(x->service_shutdown) ||
       (READ_ONCE(x->stopping) && !x->stream_error))goto out;
    if(x->streaming && READ_ONCE(x->rx_error)) {
        x->stream_error=READ_ONCE(x->rx_error);
        x->streaming=false;
        mb86H_dvb_stream_error(x->dvb,x->stream_error);
        usb_stop_rx(x);
        rc=mb86H_session_stop(&x->session,&usb_session_ops,x,x->session.source);
        dev_err(&x->intf->dev,"receive failure FW stop result=%d\n",rc);
    }
    if(x->streaming) {
        rc=mb86H_session_service(&x->session,&usb_session_ops,x);
        if(rc) {
            x->stream_error=rc?rc:-EIO;
            x->streaming=false;
            dev_err(&x->intf->dev,
                    "capture service failed result=%d\n", rc);
            mb86H_dvb_stream_error(x->dvb,x->stream_error);
            /* A transport/cleanup error cannot leave a live host reader. */
            usb_stop_rx(x);
        }
    }
    /* DVB core consumes ringbuffer.error even when a blocking read returns
     * preceding partial bytes. Keep the terminal error observable until the
     * client closes/stops; these turns never resend a USB command. */
    if(x->stream_error)mb86H_dvb_stream_error(x->dvb,x->stream_error);
    if(!READ_ONCE(x->service_shutdown) && (x->streaming || x->stream_error))
        schedule_delayed_work(&x->service_work,msecs_to_jiffies(x->stream_error?100:1));
out:
    mutex_unlock(&x->control_lock);
}

static void usb_rx_work(struct work_struct *work)
{
	struct usb_host *x = container_of(work, struct usb_host, rx_work);
	struct usb_rx_packet *packet;

	for (;;) {
		unsigned long flags;
		spin_lock_irqsave(&x->rx_lock, flags);
		if (list_empty(&x->rx_queue)) {
			spin_unlock_irqrestore(&x->rx_lock, flags);
			return;
		}
		packet = list_first_entry(&x->rx_queue, struct usb_rx_packet, node);
		list_del(&packet->node);
		--x->rx_queued;
		x->rx_stats.queued_bytes -= packet->length;
		x->rx_stats.queue_wait_ns = max(x->rx_stats.queue_wait_ns,
			ktime_get_ns() - packet->queued_ns);
		spin_unlock_irqrestore(&x->rx_lock, flags);
		if (!READ_ONCE(x->stopping) && !READ_ONCE(x->rx_error) && x->dvb) {
            u64 began = ktime_get_ns();
            int rc=mb86H_dvb_receive(x->dvb,x->session.source,
                                     packet->data,packet->length);
            x->rx_stats.delivery_ns = max(x->rx_stats.delivery_ns, ktime_get_ns() - began);
            if (!rc) x->rx_stats.delivered_bytes += packet->length;
            if(rc)usb_rx_failed(x,rc);
        } else ++x->rx_stats.dropped_packets;
		kfree(packet);
	}
}

static void usb_rx_complete(struct urb *urb)
{
	struct usb_host *x = urb->context;
	struct usb_rx_packet *packet;
	unsigned long flags;
	u64 completed_ns;

    if(READ_ONCE(x->stopping) || READ_ONCE(x->rx_error))return;
    if(urb->status) {usb_rx_failed(x,urb->status);return;}
	if (urb->actual_length) {
		spin_lock_irqsave(&x->rx_lock, flags);
		completed_ns = ktime_get_ns();
		if (!x->rx_stats.first_ns) x->rx_stats.first_ns = completed_ns;
		if (x->rx_stats.last_ns)
			x->rx_stats.completion_gap_ns = max(x->rx_stats.completion_gap_ns,
				completed_ns - x->rx_stats.last_ns);
		x->rx_stats.last_ns = completed_ns;
		x->rx_stats.bytes += urb->actual_length;
		++x->rx_stats.completions;
		spin_unlock_irqrestore(&x->rx_lock, flags);
		packet = kmalloc(struct_size(packet, data, urb->actual_length), GFP_ATOMIC);
		if (packet) {
			packet->length = urb->actual_length;
			packet->queued_ns = completed_ns;
			memcpy(packet->data, urb->transfer_buffer, packet->length);
			spin_lock_irqsave(&x->rx_lock, flags);
			if (x->rx_queued < USB_RX_QUEUE_LIMIT) {
				list_add_tail(&packet->node, &x->rx_queue);
				++x->rx_queued;
				x->rx_stats.queued_bytes += packet->length;
				x->rx_stats.queue_high_bytes = max(x->rx_stats.queue_high_bytes,
					x->rx_stats.queued_bytes);
				x->rx_stats.queue_high_packets = max(x->rx_stats.queue_high_packets,
					x->rx_queued);
				packet = NULL;
			} else ++x->rx_stats.overflows;
			spin_unlock_irqrestore(&x->rx_lock, flags);
            if(packet) {kfree(packet);usb_rx_failed(x,-ENOBUFS);}
		} else {
			spin_lock_irqsave(&x->rx_lock, flags);
			++x->rx_stats.allocation_failures;
			spin_unlock_irqrestore(&x->rx_lock, flags);
			usb_rx_failed(x,-ENOMEM);
		}
		if (x->dvb)
			schedule_work(&x->rx_work);
	}
	if (!READ_ONCE(x->stopping) && !READ_ONCE(x->rx_error)) {
		int rc = usb_submit_urb(urb, GFP_ATOMIC);
		if (rc)usb_rx_failed(x,rc);
	}
}

static int usb_start_rx(struct usb_host *x)
{
	unsigned int i;
	int rc;

	x->rx_stats = (struct usb_rx_stats){.active = true, .started_ns = ktime_get_ns()};
	for (i = 0; i < USB_RX_URBS; ++i) {
		struct usb_rx_transfer *rx = &x->rx[i];
		rx->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!rx->urb)
			return -ENOMEM;
		rx->buffer = usb_alloc_coherent(x->udev, USB_STREAM_BUFFER_BYTES,
					       GFP_KERNEL, &rx->dma);
		if (!rx->buffer)
			return -ENOMEM;
		usb_fill_bulk_urb(rx->urb, x->udev,
				  usb_rcvbulkpipe(x->udev, USB_STREAM_ENDPOINT),
				  rx->buffer, USB_STREAM_BUFFER_BYTES, usb_rx_complete, x);
		rx->urb->transfer_dma = rx->dma;
		rx->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}
	for (i = 0; i < USB_RX_URBS; ++i) {
		rc = READ_ONCE(x->rx_error);
		if (rc)
			return rc;
		rc = usb_submit_urb(x->rx[i].urb, GFP_KERNEL);
		if (rc)
			return rc;
	}
	return 0;
}

static void usb_stop_rx(struct usb_host *x)
{
	struct usb_rx_packet *packet, *next;
	unsigned long flags;
	unsigned int i;

	WRITE_ONCE(x->stopping, true);
	for (i = 0; i < USB_RX_URBS; ++i)
		if (x->rx[i].urb)
			usb_kill_urb(x->rx[i].urb);
	flush_work(&x->rx_work);
	spin_lock_irqsave(&x->rx_lock, flags);
	list_for_each_entry_safe(packet, next, &x->rx_queue, node) {
		list_del(&packet->node);
		++x->rx_stats.dropped_packets;
		kfree(packet);
	}
	x->rx_queued = 0;
	x->rx_stats.queued_bytes = 0;
	spin_unlock_irqrestore(&x->rx_lock, flags);
	for (i = 0; i < USB_RX_URBS; ++i) {
		struct usb_rx_transfer *rx = &x->rx[i];
		if (rx->buffer)
			usb_free_coherent(x->udev, USB_STREAM_BUFFER_BYTES, rx->buffer, rx->dma);
		usb_free_urb(rx->urb);
		rx->buffer = NULL;
		rx->urb = NULL;
	}
	if (x->rx_stats.active) {
		const struct usb_rx_stats *s = &x->rx_stats;
		dev_info(&x->intf->dev,
			"RX stats source=%u urbs=%u buffer=%u bytes=%llu delivered=%llu completions=%llu queue_high=%u queue_high_bytes=%zu overflow=%llu alloc_fail=%llu stop_discard=%llu rx_error=%d\n",
			x->session.source, USB_RX_URBS, USB_STREAM_BUFFER_BYTES,
			(unsigned long long)s->bytes, (unsigned long long)s->delivered_bytes,
			(unsigned long long)s->completions, s->queue_high_packets, s->queue_high_bytes,
			(unsigned long long)s->overflows, (unsigned long long)s->allocation_failures,
			(unsigned long long)s->dropped_packets, x->rx_error);
		dev_info(&x->intf->dev,
			"RX timing duration_us=%llu first_us=%llu completion_gap_us=%llu queue_wait_us=%llu delivery_us=%llu service_calls=%llu service_gap_us=%llu service_duration_us=%llu\n",
			(unsigned long long)((ktime_get_ns() - s->started_ns) / 1000),
			(unsigned long long)(s->first_ns ? (s->first_ns - s->started_ns) / 1000 : 0),
			(unsigned long long)(s->completion_gap_ns / 1000),
			(unsigned long long)(s->queue_wait_ns / 1000),
			(unsigned long long)(s->delivery_ns / 1000), (unsigned long long)s->service_calls,
			(unsigned long long)(s->service_gap_ns / 1000),
			(unsigned long long)(s->service_duration_ns / 1000));
		dev_info(&x->intf->dev,
			"RX steady service_calls=%llu service_gap_us=%llu service_duration_us=%llu gaps_10ms=%llu gaps_40ms=%llu service_total_us=%llu\n",
			(unsigned long long)s->steady_service_calls,
			(unsigned long long)(s->steady_service_gap_ns / 1000),
			(unsigned long long)(s->steady_service_duration_ns / 1000),
			(unsigned long long)s->steady_service_gaps_10ms,
			(unsigned long long)s->steady_service_gaps_40ms,
			(unsigned long long)(s->steady_service_total_ns / 1000));
		x->rx_stats.active = false;
	}
}

static void usb_register_reader(struct usb_host *x)
{
    char serial[128];
    int rc;
    snprintf(serial, sizeof(serial), "%04x:%04x:%s", x->device->vendor,
             x->device->product, x->udev->serial ? x->udev->serial : dev_name(&x->udev->dev));
    rc = mb86H_ccid_register(&usb_ccid_ops, x, x->device->name, serial);
    x->ccid_registered = !rc;
    if (rc)
        dev_warn(&x->intf->dev, "CCID reader unavailable status=%d; DVB remains available\n", rc);
}

static int usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_host *x;
	struct mb86H_device_overview overview;
	int rc, restore_rc;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;
	x = kzalloc(sizeof(*x), GFP_KERNEL);
	if (!x)
		return -ENOMEM;
    x->alias_id = -1;
	x->udev = usb_get_dev(interface_to_usbdev(intf));
	x->intf = intf;
    x->device = mb86H_device_find(le16_to_cpu(x->udev->descriptor.idVendor),
                                  le16_to_cpu(x->udev->descriptor.idProduct));
    if (!x->device) { usb_put_dev(x->udev); kfree(x); return -ENODEV; }
    memcpy(x->caps, mb86H_frontend_caps, sizeof(x->caps));
    x->caps[0].present = !!(x->device->delivery_systems & (1u << MB86H_DVB_GR));
    x->caps[1].present = !!(x->device->delivery_systems & (1u << MB86H_DVB_BSCS));
    x->session.tune_timeout_ms = x->device->vendor == USB_TORNE_VENDOR_ID ? 70000u : MB86H_INIT_TIMEOUT_MS;
    if (x->device->vendor == USB_TORNE_VENDOR_ID) {
        x->caps[0].frequency_min = 170000000;
        x->caps[0].frequency_max = 860000000;
    }
	mutex_init(&x->control_lock);
	spin_lock_init(&x->rx_lock);
	INIT_LIST_HEAD(&x->rx_queue);
	INIT_WORK(&x->rx_work, usb_rx_work);
	INIT_DELAYED_WORK(&x->service_work, usb_service_work);
	x->loader_ops = usb_loader_ops_template;
	x->loader_ops.session = &x->session;
	mb86H_card_init(&x->card, &usb_card_ops, x);
	usb_set_intfdata(intf, x);

	rc = mb86H_device_firmware_parse(x->device, &x->loader, firmware_embedded_data,
            (size_t)(firmware_embedded_end - firmware_embedded_data));
	if (rc)
		goto fail;
	mutex_lock(&x->control_lock);
	rc = mb86H_loader_start(&x->loader, &x->loader_ops, x);
	if (!rc) rc = mb86H_session_prepare(&x->session, &usb_session_ops, x, &overview);
	if (!rc) rc = usb_query_dvb_sources_locked(x, &overview);
	if (!rc) x->stream_mask = (u8)overview.stream_mask;
	mutex_unlock(&x->control_lock);
	if (rc)
		goto fail;
	if (x->loader.adopted)
		dev_info(&intf->dev, "recovered abandoned resident; pre-crash RAM snapshots are unavailable\n");
    /* Publish before DVB registration emits frontend/demux/dvr uevents. */
    rc = usb_alias_register(x);
    if (rc) goto fail;
	x->dvb = mb86H_dvb_attach(&intf->dev, x->device->name, x->caps,
			       x->stream_formats, &usb_dvb_ops, x);
	if (IS_ERR(x->dvb)) {
		rc = PTR_ERR(x->dvb);
		x->dvb = NULL;
		goto fail;
	}
	if (!x->loader.is_torne && x->stream_mask) {
        rc = usb_start_rx(x);
        if (rc) goto fail;
    }
    usb_register_reader(x);
	dev_info(&intf->dev, "%s resident initialized; DVB registered; CCID %s\n",
             x->device->name, x->ccid_registered ? "registered" : "unavailable");
	return 0;
fail:
	dev_err(&intf->dev, "initialization failed status=%d seq=%u last semantic response=%*phN; closing firmware owner\n",
		rc, x->session.sequence, 64, x->last_response);
	if (x->rx[0].urb)
		usb_stop_rx(x);
	if (x->dvb) {
		mb86H_dvb_detach(x->dvb);
		x->dvb = NULL;
	}
    usb_alias_remove(x);
	mutex_lock(&x->control_lock);
	    restore_rc = x->control_failed ? x->control_failed :
	        READ_ONCE(x->halt_attempted) ? x->halt_status :
        (x->loader.adopted || x->loader.hook_attempted || x->loader.resident_attempted || x->loader.torne_out_attempted) ?
	        mb86H_loader_close(&x->loader, &x->loader_ops, x) : 0;
	mutex_unlock(&x->control_lock);
	dev_info(&intf->dev, "probe close status=%d\n", restore_rc);
	if (restore_rc) {
		/* Do not free a live transport while it still owns modified device
		 * RAM.  Keep it bound, without DVB nodes, for disconnect/retry. */
		dev_err(&intf->dev, "probe cleanup failed: %d; retaining recovery state\n",
			restore_rc);
		return 0;
	}
	usb_set_intfdata(intf, NULL);
	kvfree(x->loader.resident_saved);
    kvfree(x->loader.torne_boot0_saved);
    kvfree(x->loader.torne_ts_ring_saved);
	usb_put_dev(x->udev);
	kfree(x);
	return rc;
}

static void usb_disconnect(struct usb_interface *intf)
{
	struct usb_host *x = usb_get_intfdata(intf);
	int rc;

	if (!x)
		return;
	usb_set_intfdata(intf, NULL);
    /* Gadget teardown drains callbacks before DVB halt restores resident or
     * the physical owner is freed. No outstanding miscdevice file lifetime. */
    if (x->ccid_registered) {
        mb86H_ccid_unregister(x);
        x->ccid_registered = false;
    }
	if (x->dvb) {
		mb86H_dvb_detach(x->dvb);
		x->dvb = NULL;
	}
    usb_alias_remove(x);
	usb_stop_rx(x);
	mutex_lock(&x->control_lock);
	rc = x->pm_error ? x->pm_error : x->control_failed ? x->control_failed :
	    READ_ONCE(x->halt_attempted) ? x->halt_status :
	    mb86H_loader_close(&x->loader, &x->loader_ops, x);
	mutex_unlock(&x->control_lock);
    dev_info(&intf->dev, "disconnect cleanup status=%d\n", rc);
	kvfree(x->loader.resident_saved);
    kvfree(x->loader.torne_boot0_saved);
    kvfree(x->loader.torne_ts_ring_saved);
	usb_put_dev(x->udev);
	kfree(x);
}

static const struct usb_device_id usb_id_table[] = {
	{ USB_DEVICE(USB_VENDOR_ID, USB_PRODUCT_ID) },
    { USB_DEVICE(USB_TORNE_VENDOR_ID, USB_TORNE_PRODUCT_ID) },
	{ USB_DEVICE(MB86H_PIX_DT295_VENDOR_ID, MB86H_PIX_DT295_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, usb_id_table);

static void usb_shutdown(struct usb_interface *intf)
{
	/* Reboot (including reboot -f) does not unbind interface drivers. Drain
	 * both clients and close while the physical USB controller is alive.
     * disconnect clears intfdata, so a later unbind is also harmless. */
	dev_info(&intf->dev, "system shutdown: draining clients and closing firmware owner\n");
    usb_disconnect(intf);
}

static int usb_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct usb_host *x = usb_get_intfdata(intf);
    if (!x) return 0;
    if (PMSG_IS_AUTO(message)) return -EBUSY;
    if (x->pm_error) return x->pm_error;
    if (x->suspended) return 0;
    /* Fence all producers under the same command lock before draining clients.
     * Do not hold this lock while waiting for their callbacks/workers. */
    mutex_lock(&x->control_lock);
    x->service_shutdown = true;
    mutex_unlock(&x->control_lock);
    cancel_delayed_work_sync(&x->service_work);
    mb86H_dvb_suspend(x->dvb);
    if (x->ccid_registered) {
        mb86H_ccid_unregister(x);
        x->ccid_registered = false;
    }
    usb_stop_rx(x);
    usb_halt(x);
    if (x->halt_status) {
        x->pm_error = x->halt_status;
        dev_err(&intf->dev, "suspend firmware cleanup failed=%d; recovery remains fenced\n", x->pm_error);
        return x->pm_error;
    }
    x->suspended = true;
    dev_info(&intf->dev, "suspended; old DVB/card sessions interrupted\n");
    return 0;
}

static int usb_resume(struct usb_interface *intf)
{
    struct usb_host *x = usb_get_intfdata(intf);
	struct mb86H_device_overview overview;
    int rc;
    if (!x) return 0;
    if (x->pm_error) return x->pm_error;
    if (!x->suspended) return 0;
    mutex_lock(&x->control_lock);
    /* A successful suspend released every old snapshot. Re-detect factory
     * boot0 vs exact retained resident; never restore old RAM after a reset. */
    mb86H_session_reset(&x->session);
    x->session.tune_timeout_ms = x->device->vendor == USB_TORNE_VENDOR_ID ? 70000u : MB86H_INIT_TIMEOUT_MS;
    x->torne_block_writes = false;
    rc = mb86H_device_firmware_parse(x->device, &x->loader, firmware_embedded_data,
        (size_t)(firmware_embedded_end - firmware_embedded_data));
    if (!rc) rc = mb86H_loader_start(&x->loader, &x->loader_ops, x);
	if (!rc) rc = mb86H_session_prepare(&x->session, &usb_session_ops, x, &overview);
	if (!rc) rc = usb_query_dvb_sources_locked(x, &overview);
	if (!rc) x->stream_mask = (u8)overview.stream_mask;
    if (!rc) mb86H_card_init(&x->card, &usb_card_ops, x);
    mutex_unlock(&x->control_lock);
    if (rc) {
        x->pm_error = rc;
        dev_err(&intf->dev, "resume validation/reinitialization failed=%d; no command replay\n", rc);
        return rc;
    }
    usb_register_reader(x);
    mutex_lock(&x->control_lock);
    x->suspended = false;
    x->service_shutdown = false;
	x->halt_attempted = false;
    x->rx_error = x->stream_error = x->halt_status = 0;
    mutex_unlock(&x->control_lock);
    mb86H_dvb_resume(x->dvb);
    dev_info(&intf->dev, "resumed; firmware ready for fresh DVB/card requests\n");
    return 0;
}

static struct usb_driver usb_host_driver = {
	.name = "mb86H_usb",
	.id_table = usb_id_table,
	.probe = usb_probe,
	.disconnect = usb_disconnect,
    .shutdown = usb_shutdown,
    .suspend = usb_suspend,
    .resume = usb_resume,
    .reset_resume = usb_resume,
};
static int __init usb_host_init(void)
{
    return usb_register(&usb_host_driver);
}

static void __exit usb_host_exit(void)
{
    usb_deregister(&usb_host_driver);
    ida_destroy(&usb_xz_alias_ids);
    ida_destroy(&usb_torne_alias_ids);
	ida_destroy(&usb_dt295_alias_ids);
}

module_init(usb_host_init);
module_exit(usb_host_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("mb86H USB resident, ISDB DVB and per-device native CCID readers");
MODULE_SOFTDEP("pre: dummy_hcd");
