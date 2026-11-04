#include "mb86H/receiver.h"
#include "mb86H/usb/libusb.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

/* One EP81 reader receives the firmware-selected delivery system. */
struct usb_stream {
    usb_stream_callback callback;
    void *opaque;
    int pending, fail_start, fail_cancel;
    unsigned starts, cancels;
    uint8_t endpoint;
};
struct usb_device { struct usb_stream stream; };

int usb_libusb_stream_start(struct usb_device *d, uint8_t endpoint,
    uint8_t *buffer, size_t size, usb_stream_callback callback, void *opaque,
    struct usb_stream **out)
{
    struct usb_stream *s = &d->stream;
    CHECK(endpoint == 0x81);
    CHECK(buffer && size && callback && out);
    *out = NULL;
    ++s->starts;
    s->endpoint = endpoint;
    if (s->pending || s->fail_start)
        return USB_SUBMIT;
    s->callback = callback;
    s->opaque = opaque;
    s->pending = 1;
    *out = s;
    return 0;
}

int usb_libusb_stream_cancel_and_drain(struct usb_stream *s)
{
    ++s->cancels;
    if (s->fail_cancel)
        return USB_TRANSFER;
    if (s->pending) {
        (void)s->callback(s->opaque, USB_TRANSFER, NULL, 0);
        s->pending = 0;
    }
    return 0;
}

static int receive_start(void *opaque, uint8_t *buffer, size_t size,
                         mb86H_receive_callback callback, void *ctx, void **handle)
{
    struct usb_stream *stream = NULL;
    int rc=usb_libusb_stream_start(opaque, USB_STREAM_ENDPOINT, buffer, size, callback, ctx, &stream);
    *handle=stream;return rc;
}
static int receive_stop(void *opaque,void *handle)
{ (void)opaque;return usb_libusb_stream_cancel_and_drain(handle); }
static const struct mb86H_transport_ops receiver_ops={
    .receive_start=receive_start,.receive_stop=receive_stop
};

static void complete(struct usb_device *d, int result,
                     const uint8_t *data, size_t size)
{
    struct usb_stream *s = &d->stream;
    CHECK(s->pending);
    (void)s->callback(s->opaque, result, data, size);
    if (result)
        s->pending = 0;
}

static void records(uint8_t *data, size_t count, uint8_t tag)
{
    for (size_t i = 0; i < count; ++i) {
        memset(data + i * 192, tag, 188);
        data[i * 192] = 0x47;
        data[i * 192 + 2] = (uint8_t)i;
        memset(data + i * 192 + 188, 0xa5, 4);
    }
}

static void frame(uint8_t output[512], unsigned channel, uint32_t sequence,
                  const uint8_t *payload, size_t size)
{
    CHECK(mb86H_ts_mux_pack(output, channel, sequence, payload, size) == 0);
}

static uint64_t read_records(struct mb86H_ts_receiver *r,
                             const uint8_t *raw, size_t count)
{
    uint8_t output[188 * 8];
    size_t size;
    uint64_t generation;
    CHECK(count <= 8);
    CHECK(mb86H_ts_receiver_read(r, output, sizeof(output), &size, &generation) == 0);
    CHECK(size == count * 188);
    for (size_t i = 0; i < count; ++i)
        CHECK(memcmp(output + i * 188, raw + i * 192, 188) == 0);
    return generation;
}

struct reader {
    struct mb86H_ts_receiver *receiver;
    uint8_t bytes[188 * 8];
    size_t size;
    uint64_t generation;
    int result;
};

static void *read_thread(void *opaque)
{
    struct reader *r = opaque;
    r->result = mb86H_ts_receiver_read(r->receiver, r->bytes,
                                     sizeof(r->bytes), &r->size, &r->generation);
    return NULL;
}

static void single_stream_lifecycle(void)
{
    struct usb_device d = {0};
    struct mb86H_transport transport={.ops=&receiver_ops,.opaque=&d};
    struct mb86H_ts_receiver *r;
    struct mb86H_ts_receiver_stats stats;
    uint8_t raw[192 * 2], f[512];
    CHECK(mb86H_ts_receiver_create(&transport, 4, &r) == 0);
    /* GR and BS use the same receiver in separate sessions, selected by FW. */
    for (unsigned source = 0; source < 2; ++source) {
        records(raw, 2, (uint8_t)(0x11 + source));
        frame(f, source, 0, raw, sizeof(raw));
        CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4) == 0);
        CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_RAW_188) == MB86H_TS_RECEIVER_BUSY);
        CHECK(d.stream.starts == source + 1 && d.stream.pending);
        complete(&d, 0, f, 191);
        complete(&d, 0, f + 191, 321);
        CHECK(read_records(r, raw, 2) == source + 1);
        CHECK(mb86H_ts_receiver_stop(r) == 0);
        CHECK(d.stream.cancels == source + 1 && !d.stream.pending);
        CHECK(mb86H_ts_receiver_stats(r, &stats) == 0 && !stats.active);
    }
    CHECK(mb86H_ts_receiver_destroy(r) == 0);
}

static void restart_and_cancel_retry(void)
{
    struct usb_device d = {0};
    struct mb86H_transport transport={.ops=&receiver_ops,.opaque=&d};
    struct mb86H_ts_receiver *r;
    uint8_t a[192 * 2], f[512];
    records(a, 2, 0x33);
    frame(f, 0, 0, a, sizeof(a));
    CHECK(mb86H_ts_receiver_create(&transport, 4, &r) == 0);
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4) == 0);
    complete(&d, 0, f, 201); /* stale fragments cannot cross sessions */
    d.stream.fail_cancel = 1;
    CHECK(mb86H_ts_receiver_stop(r) == USB_TRANSFER);
    CHECK(d.stream.pending);
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4) == MB86H_TS_RECEIVER_BUSY);
    CHECK(mb86H_ts_receiver_destroy(r) == USB_TRANSFER); /* retained for retry */
    d.stream.fail_cancel = 0;
    CHECK(mb86H_ts_receiver_stop(r) == 0);
    CHECK(!d.stream.pending);
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4) == 0);
    complete(&d, 0, f, sizeof(f));
    CHECK(read_records(r, a, 2) == 2);
    CHECK(mb86H_ts_receiver_destroy(r) == 0);
}

static void receiver_failures(void)
{
    struct usb_device d = {0};
    struct mb86H_transport transport={.ops=&receiver_ops,.opaque=&d};
    struct mb86H_ts_receiver *r;
    uint8_t raw[192 * 2], f[512], out[188 * 4], broken[192];
    size_t size;
    uint64_t generation;
    records(raw, 2, 0x44);
    CHECK(mb86H_ts_receiver_create(&transport, 2, &r) == 0);
    CHECK(mb86H_ts_receiver_start(r, 0) == MB86H_TS_RECEIVER_ARGUMENT);
    d.stream.fail_start = 1;
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_RAW_188) == USB_SUBMIT);
    CHECK(mb86H_ts_receiver_read(r, out, sizeof(out), &size, &generation) == USB_SUBMIT);
    d.stream.fail_start = 0;
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4) == 0);
    frame(f, 0, 0, raw, sizeof(raw)); complete(&d, 0, f, sizeof(f));
    frame(f, 0, 1, raw, sizeof(raw)); complete(&d, 0, f, sizeof(f));
    CHECK(read_records(r, raw, 2) == 2);
    CHECK(mb86H_ts_receiver_read(r, out, sizeof(out), &size, &generation) == MB86H_TS_RECEIVER_OVERFLOW && !size);
    CHECK(mb86H_ts_receiver_stop(r) == 0);
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4) == 0);
    frame(f, 1, 0, raw, sizeof(raw)); complete(&d, 0, f, sizeof(f));
    memcpy(broken, raw, sizeof(broken)); broken[0] = 0;
    frame(f, 1, 1, broken, sizeof(broken)); complete(&d, 0, f, sizeof(f));
    CHECK(read_records(r, raw, 2) == 3);
    {
        struct mb86H_ts_receiver_stats stats;
        CHECK(mb86H_ts_receiver_stats(r, &stats) == 0);
        CHECK(!stats.error && stats.desyncs == 1 && stats.active);
    }
    frame(f, 1, 2, raw, sizeof(raw)); complete(&d, 0, f, sizeof(f));
    CHECK(read_records(r, raw, 2) == 3);
    CHECK(mb86H_ts_receiver_stop(r) == 0);
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_RAW_188) == 0);
    complete(&d, USB_TRANSFER, NULL, 0);
    CHECK(mb86H_ts_receiver_read(r, out, sizeof(out), &size, &generation) == USB_TRANSFER && !size);
    CHECK(mb86H_ts_receiver_stop(r) == 0);
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_RAW_188) == 0);
    memset(f, 0, sizeof(f)); complete(&d, 0, f, sizeof(f));
    { /* Raw garbage is resynchronizable RF framing, not a mux error. */
        struct mb86H_ts_receiver_stats stats;
        CHECK(mb86H_ts_receiver_stats(r, &stats) == 0);
        CHECK(!stats.error && !stats.queued_packets);
    }
    CHECK(mb86H_ts_receiver_destroy(r) == 0);
}

static void discard_and_waiter(void)
{
    struct usb_device d = {0};
    struct mb86H_transport transport={.ops=&receiver_ops,.opaque=&d};
    struct mb86H_ts_receiver *r;
    struct reader reader;
    pthread_t thread;
    uint8_t raw[192 * 2], f[512];
    records(raw, 2, 0x77);
    CHECK(mb86H_ts_receiver_create(&transport, 4, &r) == 0);
    CHECK(mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4) == 0);
    frame(f, 0, 0, raw, sizeof(raw)); complete(&d, 0, f, sizeof(f));
    frame(f, 0, 1, raw, sizeof(raw)); complete(&d, 0, f, 201);
    CHECK(mb86H_ts_receiver_discard_buffered(r) == 0);
    complete(&d, 0, f + 201, sizeof(f) - 201);
    frame(f, 0, 2, raw, sizeof(raw)); complete(&d, 0, f, sizeof(f));
    CHECK(read_records(r, raw, 2) == 1);
    reader = (struct reader){ .receiver = r };
    CHECK(pthread_create(&thread, NULL, read_thread, &reader) == 0);
    CHECK(mb86H_ts_receiver_stop(r) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(reader.result == MB86H_TS_RECEIVER_STOPPED && !reader.size);
    CHECK(mb86H_ts_receiver_destroy(r) == 0);

    /* An already-waiting generation cannot consume a restarted session. */
    struct mb86H_ts_receiver_core core;
    uint8_t storage[188 * 4], out[188];
    size_t size;
    CHECK(mb86H_ts_receiver_core_init(&core, storage, 4) == 0);
    CHECK(mb86H_ts_receiver_core_start(&core, 192) == 0);
    uint64_t old = mb86H_ts_receiver_core_generation(&core);
    CHECK(mb86H_ts_receiver_core_waiting(&core, old));
    mb86H_ts_receiver_core_stop_begin(&core);
    mb86H_ts_receiver_core_stop_finish(&core, 0);
    CHECK(mb86H_ts_receiver_core_start(&core, 192) == 0);
    mb86H_ts_receiver_core_receive(&core, 0, raw, sizeof(raw));
    CHECK(mb86H_ts_receiver_core_read(&core, old, out, sizeof(out), &size) == MB86H_TS_RECEIVER_STOPPED && !size);
}

static void recoverable_rf_desync(void)
{
    struct mb86H_ts_receiver_core core;
    uint8_t storage[188*8], raw[192*2], damaged[1+192*2];
    CHECK(mb86H_ts_receiver_core_init(&core, storage, 8) == 0);
    CHECK(mb86H_ts_receiver_core_start(&core, 192) == 0);
    records(raw, 2, 0x33);
    mb86H_ts_receiver_core_receive(&core, 0, raw, sizeof(raw));
    damaged[0]=0x99;memcpy(damaged+1,raw,sizeof(raw));
    mb86H_ts_receiver_core_receive(&core, 0, damaged, sizeof(damaged));
    CHECK(!core.stats.error && core.stats.active && core.framer.desyncs==1);
    mb86H_ts_receiver_core_receive(&core, 0, raw, sizeof(raw));
    CHECK(!core.stats.error && core.used==6 && core.framer.synchronized);
    mb86H_ts_receiver_core_receive(&core, USB_TRANSFER, NULL, 0);
    CHECK(core.stats.error==USB_TRANSFER);
}

static void raw188_source_and_usb_boundaries(void)
{
    struct usb_device d = {0};
    struct mb86H_transport transport = {.ops = &receiver_ops, .opaque = &d};
    struct mb86H_ts_receiver *r;
    struct mb86H_source_descriptor descriptor = {
        .flags = MB86H_DEVICE_QUERY_PRESENT | MB86H_DEVICE_QUERY_STREAM,
        .record_size = 188, .stream_format = MB86H_STREAM_FORMAT_RAW_TS,
    };
    uint8_t raw[188 * 8], output[sizeof(raw)];
    size_t size;
    uint64_t generation;
    CHECK(!mb86H_ts_receiver_create(&transport, 16, &r));
    for (size_t i = 0; i < 8; ++i) {
        memset(raw + i * 188, (int)(0xa0 + i), 188);
        raw[i * 188] = 0x47;
        raw[i * 188 + 3] = 0xd0 | (uint8_t)i; /* Preserve scrambling and CC. */
    }
    CHECK(!mb86H_ts_receiver_start_source(r, 0, &descriptor));
    complete(&d, 0, raw, 512);
    complete(&d, 0, raw + 512, 512);
    complete(&d, 0, raw + 1024, sizeof(raw) - 1024);
    CHECK(!mb86H_ts_receiver_read(r, output, sizeof(output), &size, &generation));
    CHECK(size == sizeof(raw) && generation == 1 && !memcmp(raw, output, size));
    complete(&d, 0, raw, 97); /* Incomplete old generation cannot escape restart. */
    CHECK(!mb86H_ts_receiver_stop(r));
    CHECK(!mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_RAW_188));
    complete(&d, 0, raw, sizeof(raw));
    CHECK(!mb86H_ts_receiver_read(r, output, sizeof(output), &size, &generation));
    CHECK(size == sizeof(raw) && generation == 2 && !memcmp(raw, output, size));
    CHECK(!mb86H_ts_receiver_stop(r));
    descriptor.flags = MB86H_DEVICE_QUERY_PRESENT;
    CHECK(mb86H_ts_receiver_start_source(r, 0, &descriptor) == MB86H_TS_RECEIVER_ARGUMENT);
    descriptor.flags |= MB86H_DEVICE_QUERY_STREAM;
    descriptor.stream_format = MB86H_STREAM_FORMAT_XZM1;
    CHECK(!mb86H_ts_receiver_start_source(r, 1, &descriptor));
    uint8_t f[512];
    frame(f, 0, 0, raw, 376); /* Wrong source rejected before queue delivery. */
    complete(&d, 0, f, sizeof(f));
    CHECK(mb86H_ts_receiver_read(r, output, sizeof(output), &size, &generation) == MB86H_MUX_INVALID);
    CHECK(!size && generation == 3);
    CHECK(!mb86H_ts_receiver_stop(r));
    CHECK(!mb86H_ts_receiver_start(r, MB86H_TS_LAYOUT_XZM1_188));
    frame(f, 1, 0, raw, 376);
    complete(&d, 0, f, 200);
    complete(&d, 0, f + 200, 312);
    CHECK(!mb86H_ts_receiver_read(r, output, sizeof(output), &size, &generation));
    CHECK(size == 376 && !memcmp(raw, output, size));
    CHECK(!mb86H_ts_receiver_destroy(r));
}

int main(void)
{
    raw188_source_and_usb_boundaries();
    recoverable_rf_desync();
    single_stream_lifecycle();
    restart_and_cancel_retry();
    receiver_failures();
    discard_and_waiter();
    puts("single EP81 TS receiver lifecycle passed");
    return 0;
}
