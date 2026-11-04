/* SPDX-License-Identifier: MIT */
#include "libusb_internal.h"

static int transport_receive_start(void *opaque, uint8_t *buffer, size_t size,
                                   mb86H_receive_callback callback,
                                   void *context, void **handle)
{
    struct usb_stream *stream = NULL;
    int rc = usb_libusb_stream_start(opaque, USB_STREAM_ENDPOINT, buffer, size,
                                     callback, context, &stream);
    *handle = stream;
    return rc;
}

static int transport_receive_stop(void *opaque, void *handle)
{
    (void)opaque;
    return usb_libusb_stream_cancel_and_drain(handle);
}

static int transport_exchange(void *opaque,
                               const uint8_t request[MB86H_DEBUG_PACKET_SIZE],
                               uint8_t response[MB86H_DEBUG_PACKET_SIZE])
{
    struct usb_device *device = opaque;
    if (native_boot_only(device)) return USB_ARGUMENT;
    if (native_window_profile(device)) {
        pthread_mutex_lock(&device->control_mutex);
        int rc = native_result(usb_native_exchange(&native_ops, device, native_window_profile(device), request, response,
                                                 device->torne_block_writes));
        pthread_mutex_unlock(&device->control_mutex);
        return rc;
    }
    pthread_mutex_lock(&device->control_mutex);
    int rc = native_result(usb_xz_exchange(&native_ops, device, request, response));
    pthread_mutex_unlock(&device->control_mutex);
    return rc;
}

const struct mb86H_transport *usb_libusb_transport(struct usb_device *device)
{
    static const struct mb86H_transport_ops ops = {
        .exchange = transport_exchange,
        .receive_start = transport_receive_start,
        .receive_stop = transport_receive_stop,
    };
    if (!device || native_boot_only(device)) return NULL;
    device->transport.ops = &ops;
    device->transport.opaque = device;
    return &device->transport;
}
