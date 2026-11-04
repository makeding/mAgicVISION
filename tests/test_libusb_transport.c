#define USB_LIBUSB_TESTING
#include "../usb/userland/libusb_device.c"
#include "../usb/userland/libusb_control.c"
#include "../usb/userland/libusb_stream.c"
#include "../usb/userland/libusb_boot.c"
#include "../usb/userland/libusb.c"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

static int fail_submit(struct libusb_transfer *transfer)
{
    (void)transfer;
    return LIBUSB_ERROR_IO;
}

static pthread_mutex_t cancel_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cancel_called = PTHREAD_COND_INITIALIZER;
static int cancel_calls;

static int record_cancel(struct libusb_transfer *transfer)
{
    (void)transfer;
    pthread_mutex_lock(&cancel_mutex);
    cancel_calls++;
    pthread_cond_signal(&cancel_called);
    pthread_mutex_unlock(&cancel_mutex);
    return 0;
}

static int not_found_cancel(struct libusb_transfer *transfer)
{
    (void)transfer;
    pthread_mutex_lock(&cancel_mutex);
    cancel_calls++;
    pthread_cond_signal(&cancel_called);
    pthread_mutex_unlock(&cancel_mutex);
    return LIBUSB_ERROR_NOT_FOUND;
}

static int fail_cancel(struct libusb_transfer *transfer)
{
    (void)transfer;
    return LIBUSB_ERROR_IO;
}

static int submit_calls;

static int count_submit(struct libusb_transfer *transfer)
{
    (void)transfer;
    submit_calls++;
    return 0;
}

static int control_actual;
static enum libusb_transfer_status control_status = LIBUSB_TRANSFER_COMPLETED;

static int complete_control(struct libusb_transfer *transfer)
{
    transfer->status = control_status;
    transfer->actual_length = control_actual;
    transfer->callback(transfer);
    return 0;
}

struct captured_control {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};
static struct captured_control captured_controls[32];
static unsigned captured_count;

static int capture_control(struct libusb_transfer *transfer)
{
    struct libusb_control_setup *setup = (struct libusb_control_setup *)transfer->buffer;
    if (captured_count >= sizeof(captured_controls) / sizeof(captured_controls[0]))
        abort();
    captured_controls[captured_count++] = (struct captured_control){
        .request_type = setup->bmRequestType, .request = setup->bRequest,
        .value = libusb_le16_to_cpu(setup->wValue),
        .index = libusb_le16_to_cpu(setup->wIndex),
        .length = libusb_le16_to_cpu(setup->wLength),
    };
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    transfer->actual_length = captured_controls[captured_count - 1].length;
    transfer->callback(transfer);
    return 0;
}

static unsigned native_calls, native_observations, native_state_reads;
static int native_descriptor_result, native_download_fail;
static struct usb_libusb_native_step last_native;
static int endpoint_type(uint8_t endpoint, uint8_t *type)
{
    *type = endpoint == 0x83 ? LIBUSB_TRANSFER_TYPE_INTERRUPT : LIBUSB_TRANSFER_TYPE_BULK;
    return native_descriptor_result;
}
static int complete_native(struct libusb_transfer *transfer)
{
    ++native_calls;
    transfer->status = control_status;
    transfer->actual_length = control_actual;
    transfer->callback(transfer);
    return 0;
}
static void native_observer(void *opaque, const struct usb_libusb_native_step *step,
                             const uint8_t *data)
{
    (void)opaque; (void)data;
    last_native = *step; ++native_observations;
}
static int download_submit(struct libusb_transfer *transfer)
{
    ++native_calls;
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        const struct libusb_control_setup *s = (const void *)transfer->buffer;
        uint16_t index = libusb_le16_to_cpu(s->wIndex);
        transfer->actual_length = libusb_le16_to_cpu(s->wLength);
        if (s->bmRequestType == 0xc0) {
            uint8_t *p = transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE;
            p[0] = 0;
            p[1] = index == 0x2008 ? (native_state_reads++ ? 0x10 : 0) : 4;
        }
    } else {
        transfer->actual_length = transfer->length;
        if (transfer->endpoint == 2 && native_download_fail) {
            transfer->status = LIBUSB_TRANSFER_TIMED_OUT;
            transfer->actual_length = 7;
        }
    }
    transfer->callback(transfer);
    return 0;
}
static int always_cancelled(void *opaque) { (void)opaque; return 1; }
static int clear_halt_calls, clear_halt_result;
static int clear_endpoint(uint8_t endpoint)
{
    if (endpoint != 2) abort();
    ++clear_halt_calls;
    return clear_halt_result;
}

static int native_tests(void)
{
    struct usb_device device = {0};
    uint8_t data[544] = {'M','B','8','A','C','0','3','1'};
    size_t actual = 0;
    struct usb_boot_download_result result;
    unsigned i;
    /* Valid 32-byte native header with zero content length and 512-byte tail. */
    data[28] = 0x6f; data[29] = 0x8d; data[30] = 0x94; data[31] = 0x8e;
    pthread_mutex_init(&device.control_mutex, NULL);
    pthread_mutex_init(&device.streams_mutex, NULL);
    for (i = 0; i < USB_ENDPOINT_SLOTS; ++i)
        pthread_mutex_init(&device.streams[i].mutex, NULL);
    device.info = mb86H_device_find(0x06b8, 0x103b);
    device.native_observer = native_observer;
    test_clear_halt = clear_endpoint;
    CHECK(!native_clear_halt(&device, 2) && clear_halt_calls == 1 &&
          last_native.operation == 1 && last_native.endpoint == 2 &&
          last_native.transfer_status == -1 && !last_native.status);
    clear_halt_result = LIBUSB_ERROR_PIPE;
    CHECK(native_clear_halt(&device, 2) == LIBUSB_ERROR_PIPE && clear_halt_calls == 2 &&
          last_native.status == LIBUSB_ERROR_PIPE);
    test_clear_halt = NULL;
    test_endpoint_type = endpoint_type;
    test_submit_transfer = complete_native;
    native_calls = native_observations = 0;
    control_actual = 17; control_status = LIBUSB_TRANSFER_COMPLETED;
    CHECK(native_transmit(&device, 2, data, 17, &actual, 300) == 0 && actual == 17);
    CHECK(native_calls == 1 && native_observations == 1 && last_native.submitted &&
          last_native.endpoint == 2 && last_native.transfer_type == LIBUSB_TRANSFER_TYPE_BULK &&
          last_native.timeout_ms == 300 && last_native.actual_length == 17);
    control_actual = 16;
    CHECK(native_transmit(&device, 2, data, 17, &actual, 300) == USB_TRANSFER && actual == 16);
    CHECK(native_calls == 2 && last_native.actual_length == 16 && last_native.status == USB_TRANSFER);
    control_status = LIBUSB_TRANSFER_TIMED_OUT; control_actual = 7;
    CHECK(native_transmit(&device, 2, data, 17, &actual, 300) == -ETIMEDOUT && actual == 7);
    CHECK(native_calls == 3 && last_native.transfer_status == LIBUSB_TRANSFER_TIMED_OUT);
    control_status = LIBUSB_TRANSFER_NO_DEVICE; control_actual = 0;
    CHECK(native_transmit(&device, 2, data, 17, &actual, 300) == -ENODEV && !actual);
    control_status = LIBUSB_TRANSFER_COMPLETED; control_actual = 5;
    CHECK(native_receive(&device, 0x83, data, 6, 100) == USB_TRANSFER);
    CHECK(device.receive_status.actual_length == 5 &&
          last_native.transfer_type == LIBUSB_TRANSFER_TYPE_INTERRUPT);
    test_submit_transfer = fail_submit;
    CHECK(native_transmit(&device, 2, data, 17, &actual, 300) == LIBUSB_ERROR_IO);
    CHECK(last_native.submitted && last_native.submit_result == LIBUSB_ERROR_IO &&
          last_native.transfer_status == -1);
    native_descriptor_result = LIBUSB_ERROR_NOT_FOUND;
    CHECK(native_transmit(&device, 2, data, 17, &actual, 300) == LIBUSB_ERROR_NOT_FOUND);
    CHECK(!last_native.submitted && last_native.descriptor_result == LIBUSB_ERROR_NOT_FOUND);
    native_descriptor_result = 0;
    test_submit_transfer = download_submit;
    native_calls = native_state_reads = 0;
    device.streams[0].submitted = 1;
    CHECK(usb_libusb_download_native(&device, data, sizeof(data), NULL, &result) == -EBUSY &&
          !native_calls && !atomic_load(&device.native_taken_over));
    device.streams[0].submitted = 0;
    {
        const struct mb86H_native_download_options options = {.cancelled = always_cancelled};
        CHECK(usb_libusb_download_native(&device, data, sizeof(data), &options, &result) == -ECANCELED &&
              !native_calls && !result.download.recovery_required && !atomic_load(&device.native_taken_over));
    }
    native_download_fail = 1;
    CHECK(usb_libusb_download_native(&device, data, sizeof(data), NULL, &result) == -ETIMEDOUT);
    CHECK(result.download.recovery_required && result.download.last_actual == 7 &&
          !result.download.confirmed_bytes && atomic_load(&device.native_taken_over));
    {
        unsigned count = native_calls;
        CHECK(usb_libusb_download_native(&device, data, sizeof(data), NULL, &result) == -EALREADY &&
              native_calls == count);
    }
    /* A new owner can load from a verified cold state; no retry is implicit. */
    atomic_store(&device.native_taken_over, false);
    native_download_fail = 0; native_state_reads = 0;
    CHECK(!usb_libusb_download_native(&device, data, sizeof(data), NULL, &result));
    CHECK(result.download.stage == MB86H_NATIVE_DONE &&
          result.download.confirmed_bytes == sizeof(data) && result.boot.rawstate == 0x10 &&
          atomic_load(&device.native_taken_over));
    for (i = 0; i < USB_ENDPOINT_SLOTS; ++i)
        pthread_mutex_destroy(&device.streams[i].mutex);
    pthread_mutex_destroy(&device.control_mutex);
    pthread_mutex_destroy(&device.streams_mutex);
    test_endpoint_type = NULL;
    return 0;
}

static int ignore_callback(void *opaque, int result, const uint8_t *data, size_t size)
{
    (void)opaque;
    (void)result;
    (void)data;
    (void)size;
    return 0;
}

static int terminal_callback(void *opaque, int result, const uint8_t *data, size_t size)
{
    (void)opaque;
    (void)result;
    (void)data;
    (void)size;
    return 1;
}

struct timeout_observer {
    unsigned calls;
    int result;
    size_t size;
    uint8_t bytes[512];
};

static int observe_timeout(void *opaque, int result, const uint8_t *data, size_t size)
{
    struct timeout_observer *observer = opaque;
    if (size > sizeof(observer->bytes))
        abort();
    observer->calls++;
    observer->result = result;
    observer->size = size;
    if (size)
        memcpy(observer->bytes, data, size);
    return 0;
}

static void *cancel_thread(void *opaque)
{
    return (void *)(intptr_t)usb_libusb_stream_cancel_and_drain(opaque);
}

int main(void)
{
    struct usb_device device;
    struct usb_stream stream;
    struct usb_stream *out;
    uint8_t buffer[512] = {0};
    pthread_t thread;
    void *thread_result;

    memset(&device, 0, sizeof(device));
    pthread_mutex_init(&device.streams_mutex, NULL);
    device.streams[0].device = &device;
    pthread_mutex_init(&device.streams[0].mutex, NULL);
    pthread_cond_init(&device.streams[0].drained, NULL);
    test_submit_transfer = fail_submit;
    CHECK(usb_libusb_stream_start(&device, 0x81, buffer, sizeof(buffer),
                                  ignore_callback, NULL, &out) == USB_SUBMIT);
    CHECK(device.streams[0].transfer != NULL &&
          device.streams[0].stats.failed_transfers == 1);
    libusb_free_transfer(device.streams[0].transfer);
    pthread_cond_destroy(&device.streams[0].drained);
    pthread_mutex_destroy(&device.streams[0].mutex);
    pthread_mutex_destroy(&device.streams_mutex);

    memset(&device, 0, sizeof(device));
    memset(&stream, 0, sizeof(stream));
    stream.device = &device;
    stream.transfer = libusb_alloc_transfer(0);
    CHECK(stream.transfer != NULL);
    pthread_mutex_init(&stream.mutex, NULL);
    pthread_cond_init(&stream.drained, NULL);
    stream.submitted = 1;
    stream.callback = ignore_callback;
    stream.transfer->user_data = &stream;
    pthread_mutex_lock(&cancel_mutex);
    cancel_calls = 0;
    pthread_mutex_unlock(&cancel_mutex);
    test_cancel_transfer = record_cancel;
    CHECK(pthread_create(&thread, NULL, cancel_thread, &stream) == 0);
    pthread_mutex_lock(&cancel_mutex);
    while (!cancel_calls)
        pthread_cond_wait(&cancel_called, &cancel_mutex);
    pthread_mutex_unlock(&cancel_mutex);
    stream.transfer->status = LIBUSB_TRANSFER_CANCELLED;
    stream.transfer->actual_length = 0;
    transfer_done(stream.transfer);
    CHECK(pthread_join(thread, &thread_result) == 0);
    CHECK((intptr_t)thread_result == USB_OK);
    /* A cancel we requested is a clean stop, not a failed transfer. */
    CHECK(stream.submitted == 0 && stream.in_callback == 0 &&
          stream.stats.failed_transfers == 0 && stream.stats.last_error == USB_OK);
    libusb_free_transfer(stream.transfer);
    pthread_cond_destroy(&stream.drained);
    pthread_mutex_destroy(&stream.mutex);

    memset(&stream, 0, sizeof(stream));
    stream.device = &device;
    stream.transfer = libusb_alloc_transfer(0);
    CHECK(stream.transfer != NULL);
    pthread_mutex_init(&stream.mutex, NULL);
    pthread_cond_init(&stream.drained, NULL);
    stream.submitted = 1;
    stream.callback = ignore_callback;
    stream.transfer->user_data = &stream;
    pthread_mutex_lock(&cancel_mutex);
    cancel_calls = 0;
    pthread_mutex_unlock(&cancel_mutex);
    test_cancel_transfer = not_found_cancel;
    CHECK(pthread_create(&thread, NULL, cancel_thread, &stream) == 0);
    pthread_mutex_lock(&cancel_mutex);
    while (!cancel_calls)
        pthread_cond_wait(&cancel_called, &cancel_mutex);
    pthread_mutex_unlock(&cancel_mutex);
    stream.transfer->status = LIBUSB_TRANSFER_CANCELLED;
    transfer_done(stream.transfer);
    CHECK(pthread_join(thread, &thread_result) == 0 && (intptr_t)thread_result == USB_OK);
    libusb_free_transfer(stream.transfer);
    pthread_cond_destroy(&stream.drained);
    pthread_mutex_destroy(&stream.mutex);

    /* A CANCELLED completion we did not request is still a device failure. */
    memset(&stream, 0, sizeof(stream));
    stream.device = &device;
    stream.transfer = libusb_alloc_transfer(0);
    CHECK(stream.transfer != NULL);
    pthread_mutex_init(&stream.mutex, NULL);
    pthread_cond_init(&stream.drained, NULL);
    stream.submitted = 1;
    stream.callback = ignore_callback;
    stream.transfer->user_data = &stream;
    stream.transfer->status = LIBUSB_TRANSFER_CANCELLED;
    stream.transfer->actual_length = 0;
    transfer_done(stream.transfer);
    CHECK(stream.stats.failed_transfers == 1 && stream.stats.last_error == USB_TRANSFER);
    libusb_free_transfer(stream.transfer);
    pthread_cond_destroy(&stream.drained);
    pthread_mutex_destroy(&stream.mutex);

    memset(&stream, 0, sizeof(stream));
    stream.device = &device;
    stream.transfer = libusb_alloc_transfer(0);
    CHECK(stream.transfer != NULL);
    pthread_mutex_init(&stream.mutex, NULL);
    pthread_cond_init(&stream.drained, NULL);
    stream.submitted = 1;
    stream.callback = ignore_callback;
    stream.transfer->user_data = &stream;
    test_cancel_transfer = fail_cancel;
    CHECK(usb_libusb_stream_cancel_and_drain(&stream) == USB_TRANSFER);
    CHECK(stream.submitted == 1);
    test_cancel_transfer = record_cancel;
    pthread_mutex_lock(&cancel_mutex);
    cancel_calls = 0;
    pthread_mutex_unlock(&cancel_mutex);
    CHECK(pthread_create(&thread, NULL, cancel_thread, &stream) == 0);
    pthread_mutex_lock(&cancel_mutex);
    while (!cancel_calls)
        pthread_cond_wait(&cancel_called, &cancel_mutex);
    pthread_mutex_unlock(&cancel_mutex);
    stream.transfer->status = LIBUSB_TRANSFER_CANCELLED;
    transfer_done(stream.transfer);
    CHECK(pthread_join(thread, &thread_result) == 0 && (intptr_t)thread_result == USB_OK);
    libusb_free_transfer(stream.transfer);
    pthread_cond_destroy(&stream.drained);
    pthread_mutex_destroy(&stream.mutex);

    memset(&stream, 0, sizeof(stream));
    stream.device = &device;
    stream.transfer = libusb_alloc_transfer(0);
    CHECK(stream.transfer != NULL);
    pthread_mutex_init(&stream.mutex, NULL);
    pthread_cond_init(&stream.drained, NULL);
    stream.callback = ignore_callback;
    stream.transfer->user_data = &stream;
    submit_calls = 0;
    test_submit_transfer = count_submit;
    stream.transfer->status = LIBUSB_TRANSFER_COMPLETED;
    transfer_done(stream.transfer);
    CHECK(stream.submitted == 1 && submit_calls == 1);
    test_submit_transfer = fail_submit;
    stream.submitted = 0;
    stream.transfer->status = LIBUSB_TRANSFER_COMPLETED;
    transfer_done(stream.transfer);
    CHECK(stream.submitted == 0 && stream.stats.last_error == USB_SUBMIT);
    libusb_free_transfer(stream.transfer);
    pthread_cond_destroy(&stream.drained);
    pthread_mutex_destroy(&stream.mutex);

    /* A timed-out bulk IN still delivers its partial bytes; an empty timeout
     * simply keeps the same transfer alive.  Other transfer errors remain fatal. */
    memset(&device, 0, sizeof(device));
    atomic_init(&device.closing, false);
    memset(&stream, 0, sizeof(stream));
    stream.device = &device;
    stream.transfer = libusb_alloc_transfer(0);
    CHECK(stream.transfer != NULL);
    pthread_mutex_init(&stream.mutex, NULL);
    pthread_cond_init(&stream.drained, NULL);
    {
        uint8_t partial[512];
        struct timeout_observer observer = {0};
        for (size_t i = 0; i < sizeof(partial); ++i)
            partial[i] = (uint8_t)i;
        stream.callback = observe_timeout;
        stream.opaque = &observer;
        stream.submitted = 1;
        stream.transfer->buffer = partial;
        stream.transfer->user_data = &stream;
        submit_calls = 0;
        test_submit_transfer = count_submit;

        stream.transfer->status = LIBUSB_TRANSFER_TIMED_OUT;
        stream.transfer->actual_length = 384;
        transfer_done(stream.transfer);
        CHECK(observer.calls == 1 && observer.result == USB_OK && observer.size == 384);
        CHECK(memcmp(observer.bytes, partial, observer.size) == 0);
        CHECK(stream.submitted == 1 && submit_calls == 1 &&
              stream.stats.completed_bytes == 384 && stream.stats.completed_transfers == 1);

        stream.transfer->status = LIBUSB_TRANSFER_TIMED_OUT;
        stream.transfer->actual_length = 0;
        transfer_done(stream.transfer);
        CHECK(observer.calls == 1 && stream.submitted == 1 && submit_calls == 2 &&
              stream.stats.completed_bytes == 384 && stream.stats.completed_transfers == 2);

        stream.transfer->status = LIBUSB_TRANSFER_STALL;
        stream.transfer->actual_length = 0;
        transfer_done(stream.transfer);
        CHECK(observer.calls == 2 && observer.result == USB_TRANSFER && !observer.size);
        CHECK(!stream.submitted && submit_calls == 2 && stream.stats.failed_transfers == 1 &&
              stream.stats.last_error == USB_TRANSFER);
    }
    libusb_free_transfer(stream.transfer);
    pthread_cond_destroy(&stream.drained);
    pthread_mutex_destroy(&stream.mutex);

    memset(&device, 0, sizeof(device));
    pthread_mutex_init(&device.control_mutex, NULL);
    test_submit_transfer = complete_control;
    control_actual = 1;
    {
        uint8_t data[2] = {0};
        struct usb_control_step step = {
            .request_type = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                            LIBUSB_RECIPIENT_DEVICE,
            .request = 9, .data = data, .length = 2,
        };
        CHECK(usb_libusb_control(&device, &step, 1, 100) == USB_OK);
        CHECK(step.actual_length == 1);
        step.request_type = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                            LIBUSB_RECIPIENT_DEVICE;
        CHECK(usb_libusb_control(&device, &step, 1, 100) == USB_TRANSFER);
        CHECK(step.actual_length == 1);
        CHECK(step.submit_result == 0 && step.transfer_status == LIBUSB_TRANSFER_COMPLETED);
        control_actual = 0;
        control_status = LIBUSB_TRANSFER_TIMED_OUT;
        CHECK(usb_libusb_control(&device, &step, 1, 100) == USB_TRANSFER);
        CHECK(step.transfer_status == LIBUSB_TRANSFER_TIMED_OUT && step.actual_length == 0);
        control_status = LIBUSB_TRANSFER_STALL;
        CHECK(usb_libusb_control(&device, &step, 1, 100) == USB_TRANSFER);
        CHECK(step.transfer_status == LIBUSB_TRANSFER_STALL);
        test_submit_transfer = fail_submit;
        CHECK(usb_libusb_control(&device, &step, 1, 100) == USB_TRANSFER);
        CHECK(step.submit_result == LIBUSB_ERROR_IO && step.transfer_status == -1);
        control_status = LIBUSB_TRANSFER_COMPLETED;
    }
    pthread_mutex_destroy(&device.control_mutex);

    /* A decoder-level terminal error prevents the automatic re-submit while
     * retaining ownership for the normal cancel/drain path. */
    memset(&stream, 0, sizeof(stream));
    stream.device = &device;
    stream.transfer = libusb_alloc_transfer(0);
    CHECK(stream.transfer != NULL);
    pthread_mutex_init(&stream.mutex, NULL);
    pthread_cond_init(&stream.drained, NULL);
    stream.submitted = 1;
    stream.callback = terminal_callback;
    stream.transfer->user_data = &stream;
    submit_calls = 0;
    test_submit_transfer = count_submit;
    stream.transfer->status = LIBUSB_TRANSFER_COMPLETED;
    stream.transfer->actual_length = 0;
    transfer_done(stream.transfer);
    CHECK(!stream.submitted && stream.cancelling && submit_calls == 0);
    libusb_free_transfer(stream.transfer);
    pthread_cond_destroy(&stream.drained);
    pthread_mutex_destroy(&stream.mutex);

    /* The two endpoint slots keep independent buffers and lifecycle state. */
    memset(&device, 0, sizeof(device));
    atomic_init(&device.closing, false);
    pthread_mutex_init(&device.streams_mutex, NULL);
    for (unsigned index = 0; index < USB_MAX_STREAMS; ++index) {
        device.streams[index].device = &device;
        pthread_mutex_init(&device.streams[index].mutex, NULL);
        pthread_cond_init(&device.streams[index].drained, NULL);
    }
    {
        uint8_t second_buffer[512] = {0};
        struct usb_stream *first;
        struct usb_stream *second;
        submit_calls = 0;
        test_submit_transfer = count_submit;
        CHECK(usb_libusb_stream_start(&device, 0x81, buffer, sizeof(buffer),
                                      ignore_callback, NULL, &first) == USB_OK);
        CHECK(usb_libusb_stream_start(&device, 0x84, second_buffer, sizeof(second_buffer),
                                      ignore_callback, NULL, &second) == USB_OK);
        CHECK(first != second && first->transfer->buffer == buffer &&
              second->transfer->buffer == second_buffer && submit_calls == 2);
        CHECK(usb_libusb_stream_start(&device, 0x81, buffer, sizeof(buffer),
                                      ignore_callback, NULL, &out) == USB_SUBMIT);
        CHECK(out == NULL);
        first->cancelling = 1;
        first->transfer->status = LIBUSB_TRANSFER_CANCELLED;
        transfer_done(first->transfer);
        CHECK(usb_libusb_stream_cancel_and_drain(first) == USB_OK);
        CHECK(second->submitted == 1 && second->stats.failed_transfers == 0);
        CHECK(usb_libusb_stream_start(&device, 0x81, buffer, sizeof(buffer),
                                      ignore_callback, NULL, &out) == USB_OK);
        CHECK(out == first && first->submitted == 1);
        for (unsigned index = 0; index < USB_MAX_STREAMS; ++index) {
            struct usb_stream *active = &device.streams[index];
            active->cancelling = 1;
            active->transfer->status = LIBUSB_TRANSFER_CANCELLED;
            transfer_done(active->transfer);
            CHECK(usb_libusb_stream_cancel_and_drain(active) == USB_OK);
        }
    }
    for (unsigned index = 0; index < USB_MAX_STREAMS; ++index) {
        libusb_free_transfer(device.streams[index].transfer);
        pthread_cond_destroy(&device.streams[index].drained);
        pthread_mutex_destroy(&device.streams[index].mutex);
    }
    pthread_mutex_destroy(&device.streams_mutex);

    /* Torne stages C4600 word-zero last under the one control owner, then
     * dispatches the proven native BC envelope. */
    memset(&device, 0, sizeof(device));
    device.vendor = USB_TORNE_VENDOR_ID;
    device.product = USB_TORNE_PRODUCT_ID;
    pthread_mutex_init(&device.control_mutex, NULL);
    captured_count = 0;
    test_submit_transfer = capture_control;
    {
        uint8_t request[MB86H_DEBUG_PACKET_SIZE] = {0};
        uint8_t response[MB86H_DEBUG_PACKET_SIZE] = {0};
        /* Generic scalar staging restores use the same word-zero-last
         * ordering as exchanges after the loader disables block OUT. */
        CHECK(usb_libusb_boot0_write(&device, MB86H_BOOT0_STAGING_ADDRESS,
                                     request, sizeof(request)) == USB_OK);
        CHECK(captured_count == 16 && captured_controls[0].index == 0x4604 &&
              captured_controls[14].index == 0x463c && captured_controls[15].index == 0x4600);
        captured_count = 0;
        CHECK(transport_exchange(&device, request, response) == USB_OK);
        CHECK(captured_count == 17);
        CHECK(captured_controls[0].request_type == 0x40 &&
              captured_controls[0].request == USB_WINDOW_VENDOR_REQUEST &&
              captured_controls[0].index == 0x4604 && captured_controls[0].length == 4);
        CHECK(captured_controls[15].index == 0x4600 && captured_controls[15].length == 4);
        CHECK(captured_controls[16].request_type == 0xc0 &&
              captured_controls[16].value == MB86H_TORNE_BOOT0_DISPATCH_VALUE &&
              captured_controls[16].index == MB86H_TORNE_BOOT0_DISPATCH_INDEX &&
              captured_controls[16].length == MB86H_DEBUG_PACKET_SIZE);
        CHECK(usb_libusb_set_block_writes(&device, true) == USB_OK);
        captured_count = 0;
        CHECK(transport_exchange(&device, request, response) == USB_OK);
        CHECK(captured_count == 2 && captured_controls[0].request_type == 0x40 &&
              captured_controls[0].index == 0x4600 && captured_controls[0].length == 64);
    }
    pthread_mutex_destroy(&device.control_mutex);
    /* Native bootstrap is selected by the shared device registry. It uses
     * this same control owner without acquiring a resident/TS transport. */
    memset(&device, 0, sizeof(device));
    /* Unverified sibling keeps the native-observation-only boundary. */
    device.info = mb86H_device_find(0x06b8, 0x103c);
    CHECK(device.info && supported_target(0x06b8, 0x103c));
    CHECK(!supported_target(0x06b8, 0x102b));
    pthread_mutex_init(&device.control_mutex, NULL);
    captured_count = 0;
    test_submit_transfer = capture_control;
    {
        struct usb_boot_result boot;
        uint8_t bytes[65] = {0};
        CHECK(usb_libusb_prepare_boot0(&device, &boot) == USB_OK);
        CHECK(boot.stage == USB_BOOT_STAGE_BOOT0_READY && captured_count == 1);
        CHECK(captured_controls[0].request_type == 0xc0 &&
              captured_controls[0].value == 0x0800 &&
              captured_controls[0].index == 0x2008 && captured_controls[0].length == 2);
        captured_count = 0;
        CHECK(usb_libusb_boot0_read(&device, 0xe0000, bytes, sizeof(bytes)) == USB_OK);
        CHECK(captured_count == 2 && captured_controls[0].value == 0x0e00 &&
              captured_controls[0].index == 0 && captured_controls[0].length == 64 &&
              captured_controls[1].index == 64 && captured_controls[1].length == 1);
        CHECK(usb_libusb_transport(&device) == NULL);
        CHECK(usb_libusb_boot0_write(&device, 0xe0000, bytes, 4) == USB_ARGUMENT);
        CHECK(captured_count == 2);
        test_submit_transfer = complete_control;
        control_actual = 1;
        CHECK(usb_libusb_boot0_read(&device, 0xe0000, bytes, sizeof(bytes)) == USB_TRANSFER);
    }
    pthread_mutex_destroy(&device.control_mutex);
    CHECK(!native_tests());
    puts("libusb transport lifecycle tests passed");
    return 0;
}
