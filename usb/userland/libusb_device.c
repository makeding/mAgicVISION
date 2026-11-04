#include "libusb_internal.h"

static bool supported_target(uint16_t vendor, uint16_t product)
{
    return mb86H_device_find(vendor, product) != NULL;
}

bool native_boot_only(const struct usb_device *device)
{
    return atomic_load(&device->native_taken_over) ||
        (device->info && device->info->runtime_protocol == MB86H_RUNTIME_NONE);
}

/* Resolve an endpoint from the claimed interface's default alternate.
 * The caller/device binding supplies its endpoint; transport owns its type. */
int receive_type(libusb_device *device, uint8_t endpoint, uint8_t *type)
{
    struct libusb_config_descriptor *config = NULL;
    int rc = libusb_get_active_config_descriptor(device, &config);
    *type = 0;
    if (rc) return rc;
    for (unsigned i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int j = 0; j < interface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *alt = &interface->altsetting[j];
            if (alt->bInterfaceNumber || alt->bAlternateSetting) continue;
            for (unsigned k = 0; k < alt->bNumEndpoints; ++k) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                uint8_t candidate = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                if (ep->bEndpointAddress == endpoint &&
                    (candidate == LIBUSB_TRANSFER_TYPE_BULK ||
                     candidate == LIBUSB_TRANSFER_TYPE_INTERRUPT)) *type = candidate;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return *type ? USB_OK : LIBUSB_ERROR_NOT_FOUND;
}

bool is_torne(const struct usb_device *device)
{
    return device->vendor == USB_TORNE_VENDOR_ID &&
           device->product == USB_TORNE_PRODUCT_ID;
}

const struct usb_native_window_profile *native_window_profile(
    const struct usb_device *device)
{
    if (device && device->info && device->info->usb_boot && device->info->usb_boot->window)
        return device->info->usb_boot->window;
    return device && is_torne(device) ? &usb_torne_window_profile : NULL;
}
static void *event_loop(void *opaque)
{
    struct usb_device *device = opaque;
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 };

    while (!atomic_load(&device->closing))
        (void)libusb_handle_events_timeout_completed(device->context, &timeout, NULL);
    return NULL;
}

int usb_libusb_count_target(uint16_t vendor, uint16_t product, unsigned *count)
{
    libusb_context *context = NULL;
    libusb_device **devices;
    ssize_t device_count;
    unsigned matches = 0;
    ssize_t index;

    if (!count || !supported_target(vendor, product))
        return USB_ARGUMENT;
    *count = 0;
    if (libusb_init(&context) != 0)
        return USB_TRANSFER;
    device_count = libusb_get_device_list(context, &devices);
    if (device_count < 0) {
        libusb_exit(context);
        return USB_TRANSFER;
    }
    for (index = 0; index < device_count; ++index) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(devices[index], &descriptor) == 0 &&
            descriptor.idVendor == vendor && descriptor.idProduct == product)
            matches++;
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    *count = matches;
    return USB_OK;
}

int usb_libusb_count_devices(unsigned *count)
{
    return usb_libusb_count_target(USB_VENDOR_ID, USB_PRODUCT_ID, count);
}

int usb_libusb_open_target(struct usb_device **out, uint16_t vendor,
                           uint16_t product)
{
    struct usb_device *device;
    libusb_device **devices;
    ssize_t device_count;
    ssize_t index;
    unsigned matches = 0;
    int failure = USB_NOT_FOUND;
    unsigned initialized_streams = 0;
    int control_initialized = 0;
    int streams_initialized = 0;

    if (!out || !supported_target(vendor, product))
        return USB_ARGUMENT;
    *out = NULL;
    device = calloc(1, sizeof(*device));
    if (!device || libusb_init(&device->context) != 0) {
        free(device);
        return USB_TRANSFER;
    }
    device_count = libusb_get_device_list(device->context, &devices);
    if (device_count < 0) {
        failure = USB_TRANSFER;
        goto failed;
    }
    for (index = 0; index < device_count; ++index) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(devices[index], &descriptor) == 0 &&
            descriptor.idVendor == vendor && descriptor.idProduct == product)
            matches++;
    }
    if (matches == 1) {
        for (index = 0; index < device_count; ++index) {
            struct libusb_device_descriptor descriptor;
            if (libusb_get_device_descriptor(devices[index], &descriptor) == 0 &&
                descriptor.idVendor == vendor && descriptor.idProduct == product) {
                if (libusb_open(devices[index], &device->handle) != 0) {
                    matches = 0;
                    failure = USB_OPEN;
                } else {
                    device->identity = (struct usb_libusb_identity){
                        vendor, product, descriptor.bcdDevice,
                        libusb_get_bus_number(devices[index]),
                        libusb_get_device_address(devices[index]),
                    };
                }
                break;
            }
        }
    }
    libusb_free_device_list(devices, 1);
    if (matches != 1) {
        if (matches > 1)
            failure = USB_AMBIGUOUS;
        goto failed;
    }
    if (libusb_claim_interface(device->handle, 0) != 0) {
        failure = USB_OPEN;
        goto failed;
    }
    failure = USB_TRANSFER;
    if (pthread_mutex_init(&device->control_mutex, NULL) != 0)
        goto failed_claimed;
    control_initialized = 1;
    if (pthread_mutex_init(&device->streams_mutex, NULL) != 0)
        goto failed_claimed;
    streams_initialized = 1;
    atomic_init(&device->closing, false);
    atomic_init(&device->native_taken_over, false);
    device->vendor = vendor;
    device->product = product;
    device->info = mb86H_device_find(vendor, product);
    for (index = 0; index < USB_ENDPOINT_SLOTS; ++index) {
        device->streams[index].device = device;
        if (pthread_mutex_init(&device->streams[index].mutex, NULL) != 0)
            goto failed_claimed;
        if (pthread_cond_init(&device->streams[index].drained, NULL) != 0) {
            pthread_mutex_destroy(&device->streams[index].mutex);
            goto failed_claimed;
        }
        initialized_streams++;
    }
    if (pthread_create(&device->event_thread, NULL, event_loop, device) != 0)
        goto failed_claimed;
    device->event_started = 1;
    *out = device;
    return USB_OK;
failed_claimed:
    for (index = 0; index < initialized_streams; ++index) {
        pthread_cond_destroy(&device->streams[index].drained);
        pthread_mutex_destroy(&device->streams[index].mutex);
    }
    if (streams_initialized)
        pthread_mutex_destroy(&device->streams_mutex);
    if (control_initialized)
        pthread_mutex_destroy(&device->control_mutex);
    (void)libusb_release_interface(device->handle, 0);
failed:
    if (device->handle)
        libusb_close(device->handle);
    libusb_exit(device->context);
    free(device);
    return failure;
}

int usb_libusb_open(struct usb_device **out)
{
    return usb_libusb_open_target(out, USB_VENDOR_ID, USB_PRODUCT_ID);
}

int usb_libusb_get_identity(struct usb_device *device, struct usb_libusb_identity *out)
{
    if (!device || !out) return USB_ARGUMENT;
    *out = device->identity;
    return USB_OK;
}

int usb_libusb_get_receive_status(struct usb_device *device,
                                 struct usb_libusb_receive_status *out)
{
    if (!device || !out) return USB_ARGUMENT;
    *out = device->receive_status;
    return USB_OK;
}

int usb_libusb_observe_control(struct usb_device *device,
                              usb_control_observer observer, void *opaque)
{
    if (!device || pthread_equal(pthread_self(), device->event_thread)) return USB_ARGUMENT;
    pthread_mutex_lock(&device->control_mutex);
    device->observer = observer;
    device->observer_opaque = opaque;
    pthread_mutex_unlock(&device->control_mutex);
    return USB_OK;
}

int usb_libusb_observe_native(struct usb_device *device,
                             usb_native_observer observer, void *opaque)
{
    if (!device || pthread_equal(pthread_self(), device->event_thread)) return USB_ARGUMENT;
    pthread_mutex_lock(&device->control_mutex);
    device->native_observer = observer;
    device->native_observer_opaque = opaque;
    pthread_mutex_unlock(&device->control_mutex);
    return USB_OK;
}

int usb_libusb_set_block_writes(struct usb_device *device, bool enabled)
{
    if (!device || !native_window_profile(device))
        return USB_ARGUMENT;
    pthread_mutex_lock(&device->control_mutex);
    device->torne_block_writes = enabled;
    pthread_mutex_unlock(&device->control_mutex);
    return USB_OK;
}

int usb_libusb_close(struct usb_device *device)
{
    unsigned index;
    if (!device)
        return USB_ARGUMENT;
    if (pthread_equal(pthread_self(), device->event_thread))
        return USB_CLOSED;
    for (index = 0; index < USB_ENDPOINT_SLOTS; ++index)
        if (device->streams[index].transfer &&
            usb_libusb_stream_cancel_and_drain(&device->streams[index]) != USB_OK)
            return USB_TRANSFER;
    atomic_store(&device->closing, true);
    if (device->event_started)
        pthread_join(device->event_thread, NULL);
    for (index = 0; index < USB_ENDPOINT_SLOTS; ++index) {
        struct usb_stream *stream = &device->streams[index];
        if (stream->transfer)
            libusb_free_transfer(stream->transfer);
        pthread_cond_destroy(&stream->drained);
        pthread_mutex_destroy(&stream->mutex);
    }
    (void)libusb_release_interface(device->handle, 0);
    libusb_close(device->handle);
    pthread_mutex_destroy(&device->streams_mutex);
    pthread_mutex_destroy(&device->control_mutex);
    libusb_exit(device->context);
    free(device);
    return USB_OK;
}

int usb_libusb_reset(struct usb_device *device)
{
    int result;
    if (!device)
        return USB_ARGUMENT;
    pthread_mutex_lock(&device->control_mutex);
    result = libusb_reset_device(device->handle);
    pthread_mutex_unlock(&device->control_mutex);
    return result == 0 ? USB_OK : USB_TRANSFER;
}
