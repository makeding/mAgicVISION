#include "libusb_internal.h"

static void transfer_done(struct libusb_transfer *transfer)
{
    struct usb_stream *stream = transfer->user_data;
    usb_stream_callback callback;
    void *opaque;
    int result = USB_OK;
    int length = 0;
    int resubmit_failed = 0;
    int callback_stop = 0;

    pthread_mutex_lock(&stream->mutex);
    stream->submitted = 0;
    stream->in_callback = 1;
    if (transfer->status == LIBUSB_TRANSFER_CANCELLED && stream->cancelling) {
        /* Our own cancel-and-drain ended this transfer; not a device fault.
         * Bytes already received before the cancel are still delivered. */
        length = transfer->actual_length;
        stream->stats.completed_bytes += (size_t)length;
    } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED ||
               transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
        length = transfer->actual_length;
        stream->stats.completed_bytes += (size_t)length;
        stream->stats.completed_transfers++;
    } else {
        result = USB_TRANSFER;
        stream->stats.failed_transfers++;
        stream->stats.last_error = result;
    }
    callback = stream->callback;
    opaque = stream->opaque;
    pthread_mutex_unlock(&stream->mutex);

    /* Bulk timeout preserves actual_length bytes. Deliver that partial batch
     * instead of waiting indefinitely for a full host buffer or short packet. */
    if (callback && (result || length || transfer->status == LIBUSB_TRANSFER_COMPLETED))
        callback_stop = callback(opaque, result, result ? NULL : transfer->buffer,
                                 (size_t)length) != 0;

    pthread_mutex_lock(&stream->mutex);
    if (callback_stop)
        stream->cancelling = 1;
    if (!result && !stream->cancelling &&
        !atomic_load(&stream->device->closing)) {
        if (submit_transfer(stream->transfer) == 0) {
            stream->submitted = 1;
        } else {
            stream->stats.failed_transfers++;
            stream->stats.last_error = USB_SUBMIT;
            resubmit_failed = 1;
        }
    }
    callback = stream->callback;
    opaque = stream->opaque;
    pthread_mutex_unlock(&stream->mutex);
    if (resubmit_failed && callback)
        (void)callback(opaque, USB_SUBMIT, NULL, 0);
    pthread_mutex_lock(&stream->mutex);
    stream->in_callback = 0;
    pthread_cond_broadcast(&stream->drained);
    pthread_mutex_unlock(&stream->mutex);
}
int usb_libusb_stream_cancel_and_drain(struct usb_stream *stream)
{
    int submit_state;
    int cancel_result;

    if (!stream)
        return USB_ARGUMENT;
    if (pthread_equal(pthread_self(), stream->device->event_thread))
        return USB_CLOSED;
    pthread_mutex_lock(&stream->mutex);
    submit_state = stream->submitted;
    if (stream->cancel_requested && submit_state) {
        while (stream->submitted || stream->in_callback)
            pthread_cond_wait(&stream->drained, &stream->mutex);
        pthread_mutex_unlock(&stream->mutex);
        return USB_OK;
    }
    stream->cancelling = 1;
    cancel_result = submit_state ? cancel_transfer(stream->transfer) : 0;
    if (cancel_result != 0 && cancel_result != LIBUSB_ERROR_NOT_FOUND) {
        stream->stats.last_error = USB_TRANSFER;
        pthread_mutex_unlock(&stream->mutex);
        return USB_TRANSFER;
    }
    if (submit_state)
        stream->cancel_requested = 1;
    while (stream->submitted || stream->in_callback)
        pthread_cond_wait(&stream->drained, &stream->mutex);
    pthread_mutex_unlock(&stream->mutex);
    return USB_OK;
}
int usb_libusb_stream_start(struct usb_device *device, uint8_t endpoint,
                         uint8_t *buffer, size_t buffer_size,
                         usb_stream_callback callback, void *opaque,
                         struct usb_stream **out)
{
    struct usb_stream *stream;
    unsigned index;
    int rc;
    if (!out)
        return USB_ARGUMENT;
    *out = NULL;
    if (!device || !usb_endpoint_slot(endpoint, &index) || !buffer || !buffer_size ||
        buffer_size > INT_MAX || !callback || atomic_load(&device->closing))
        return USB_ARGUMENT;
    if (native_boot_only(device)) return USB_ARGUMENT;
    pthread_mutex_lock(&device->streams_mutex);
    if (native_boot_only(device)) {
        pthread_mutex_unlock(&device->streams_mutex);
        return USB_ARGUMENT;
    }
    stream = &device->streams[index];
    pthread_mutex_lock(&stream->mutex);
    if (stream->submitted || stream->in_callback) {
        pthread_mutex_unlock(&stream->mutex);
        pthread_mutex_unlock(&device->streams_mutex);
        return USB_SUBMIT;
    }
    if (!stream->transfer)
        stream->transfer = libusb_alloc_transfer(0);
    if (!stream->transfer) {
        pthread_mutex_unlock(&stream->mutex);
        pthread_mutex_unlock(&device->streams_mutex);
        return USB_SUBMIT;
    }
    stream->cancelling = 0;
    stream->cancel_requested = 0;
    stream->callback = callback;
    stream->opaque = opaque;
    libusb_fill_bulk_transfer(stream->transfer, device->handle, endpoint, buffer,
                              (int)buffer_size, transfer_done, stream, USB_STREAM_FLUSH_MS);
    stream->submitted = 1;
    rc = submit_transfer(stream->transfer);
    if (rc != 0) {
        stream->submitted = 0;
        stream->stats.failed_transfers++;
        stream->stats.last_error = USB_SUBMIT;
        pthread_mutex_unlock(&stream->mutex);
        pthread_mutex_unlock(&device->streams_mutex);
        return USB_SUBMIT;
    }
    pthread_mutex_unlock(&stream->mutex);
    pthread_mutex_unlock(&device->streams_mutex);
    *out = stream;
    return USB_OK;
}

struct usb_stream_stats usb_libusb_stream_stats(struct usb_stream *stream)
{
    struct usb_stream_stats empty = {0};
    if (!stream)
        return empty;
    pthread_mutex_lock(&stream->mutex);
    empty = stream->stats;
    pthread_mutex_unlock(&stream->mutex);
    return empty;
}
