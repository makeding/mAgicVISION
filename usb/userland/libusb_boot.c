#include "libusb_internal.h"

static int native_control(void *opaque, const struct usb_window_control *control,
                           void *data, unsigned timeout)
{
    struct usb_device *device = opaque;
    struct usb_control_step step = {
        .request_type = control->request_type, .request = control->request,
        .value = control->value, .index = control->index,
        .data = data, .length = control->length,
    };
    int rc;
    if (device->download_options && device->download_options->cancelled &&
        device->download_options->cancelled(device->download_options->opaque))
        return -ECANCELED;
    rc = control_locked(device, &step, 1, timeout);
    if (step.submit_result) return step.submit_result;
    if (step.transfer_status == LIBUSB_TRANSFER_TIMED_OUT) return -ETIMEDOUT;
    if (step.transfer_status == LIBUSB_TRANSFER_NO_DEVICE) return -ENODEV;
    return rc || step.actual_length != control->length ? USB_TRANSFER : USB_OK;
}
const struct usb_native_ops native_ops = {.control = native_control};
int native_result(int rc)
{
    return rc == -EINVAL ? USB_ARGUMENT : rc == -EIO ? USB_TRANSFER : rc;
}

static int native_endpoint(struct usb_device *device, uint8_t endpoint, void *data,
                            size_t size, size_t *actual, unsigned timeout_ms)
{
    struct control_wait wait = {0};
    struct libusb_transfer *transfer;
    struct usb_libusb_native_step step = {
        .endpoint = endpoint, .transfer_status = -1,
        .requested_length = (unsigned)size, .timeout_ms = timeout_ms,
    };
    uint8_t type;
    int rc;
    bool reading = (endpoint & LIBUSB_ENDPOINT_IN) != 0;
    if (!data || !actual || !size || size > 0x10000u || !timeout_ms)
        return USB_ARGUMENT;
    *actual = 0;
    if (device->download_options && device->download_options->cancelled &&
        device->download_options->cancelled(device->download_options->opaque))
        return -ECANCELED;
    type = 0;
#ifdef USB_LIBUSB_TESTING
    if (test_endpoint_type) rc = test_endpoint_type(endpoint, &type);
    else
#endif
    rc = receive_type(libusb_get_device(device->handle), endpoint, &type);
    step.transfer_type = type;
    step.descriptor_result = rc;
    if (rc) goto observed;
    if (!reading && type != LIBUSB_TRANSFER_TYPE_BULK) {
        rc = step.descriptor_result = LIBUSB_ERROR_NOT_SUPPORTED; goto observed;
    }
    transfer = libusb_alloc_transfer(0);
    if (!transfer) { rc = -ENOMEM; goto observed; }
    if (pthread_mutex_init(&wait.mutex, NULL)) {
        libusb_free_transfer(transfer); rc = -ENOMEM; goto observed;
    }
    if (pthread_cond_init(&wait.complete, NULL)) {
        pthread_mutex_destroy(&wait.mutex);
        libusb_free_transfer(transfer); rc = -ENOMEM; goto observed;
    }
    if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
        libusb_fill_interrupt_transfer(transfer, device->handle, endpoint,
                                        data, (int)size, control_done, &wait, timeout_ms);
    else
        libusb_fill_bulk_transfer(transfer, device->handle, endpoint,
                                   data, (int)size, control_done, &wait, timeout_ms);
    step.submitted = 1;
    rc = submit_transfer(transfer);
    step.submit_result = rc;
    if (!rc) {
        pthread_mutex_lock(&wait.mutex);
        while (!wait.done) pthread_cond_wait(&wait.complete, &wait.mutex);
        pthread_mutex_unlock(&wait.mutex);
        step.transfer_status = wait.status;
        step.actual_length = wait.actual;
        if (wait.actual > 0 && (size_t)wait.actual <= size) *actual = (size_t)wait.actual;
        if (wait.status == LIBUSB_TRANSFER_TIMED_OUT) rc = -ETIMEDOUT;
        else if (wait.status == LIBUSB_TRANSFER_NO_DEVICE) rc = -ENODEV;
        else if (wait.status != LIBUSB_TRANSFER_COMPLETED || wait.actual < 0 ||
                 (size_t)wait.actual != size) rc = USB_TRANSFER;
    }
    pthread_cond_destroy(&wait.complete);
    pthread_mutex_destroy(&wait.mutex);
    libusb_free_transfer(transfer);
observed:
    step.status = rc;
    if (reading) {
        device->receive_status = (struct usb_libusb_receive_status){
            .submitted = step.submitted,
            .submit_result = step.submit_result, .transfer_status = step.transfer_status,
            .actual_length = step.actual_length, .descriptor_result = step.descriptor_result,
            .requested_length = (unsigned)size, .timeout_ms = timeout_ms,
            .endpoint = endpoint, .transfer_type = step.transfer_type,
        };
        if (*actual && *actual <= sizeof(device->receive_status.bytes))
            memcpy(device->receive_status.bytes, data, *actual);
    }
    if (device->native_observer)
        device->native_observer(device->native_observer_opaque, &step,
                                reading && !*actual ? NULL : data);
    return rc;
}

static int native_receive(void *opaque, uint8_t endpoint, void *data,
                           size_t size, unsigned timeout_ms)
{
    size_t actual;
    if (!(endpoint & 0x80u) || size > 64u) return USB_ARGUMENT;
    return native_endpoint(opaque, endpoint, data, size, &actual, timeout_ms);
}

static int native_transmit(void *opaque, uint8_t endpoint, const void *data,
                            size_t size, size_t *actual, unsigned timeout_ms)
{
    if (endpoint & 0x80u) return USB_ARGUMENT;
    return native_endpoint(opaque, endpoint, (void *)data, size, actual, timeout_ms);
}

static int native_clear_halt(void *opaque, uint8_t endpoint)
{
    struct usb_device *device = opaque;
    struct usb_libusb_native_step step = {
        .operation = 1, .endpoint = endpoint, .transfer_status = -1,
    };
    int rc;
    if (device->download_options && device->download_options->cancelled &&
        device->download_options->cancelled(device->download_options->opaque))
        return -ECANCELED;
#ifdef USB_LIBUSB_TESTING
    if (test_clear_halt) rc = test_clear_halt(endpoint);
    else
#endif
    rc = libusb_clear_halt(device->handle, endpoint);
    step.submitted = 1;
    step.submit_result = step.status = rc;
    if (device->native_observer)
        device->native_observer(device->native_observer_opaque, &step, NULL);
    return rc;
}

static uint32_t native_now_ms(void *opaque)
{
    struct timespec now;
    (void)opaque;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint32_t)((unsigned long long)now.tv_sec * 1000u + now.tv_nsec / 1000000u);
}
static void native_delay_ms(void *opaque, unsigned ms)
{
    struct timespec delay = {ms / 1000u, (long)(ms % 1000u) * 1000000L};
    (void)opaque;
    (void)nanosleep(&delay, NULL);
}
static const struct usb_boot_io native_boot_io = {
    .native = {.control = native_control}, .receive = native_receive,
    .transmit = native_transmit, .clear_halt = native_clear_halt,
    .now_ms = native_now_ms, .delay_ms = native_delay_ms,
};

int usb_libusb_download_native(struct usb_device *device, const uint8_t *image,
    size_t size, const struct mb86H_native_download_options *options,
    struct usb_boot_download_result *result)
{
    int rc = 0;
    unsigned i;
    if (!result) return USB_ARGUMENT;
    memset(result, 0, sizeof(*result));
    result->download.image_size = size;
    if (!device || !device->info || !device->info->usb_boot ||
        !device->info->usb_boot->download ||
        pthread_equal(pthread_self(), device->event_thread)) {
        result->download.stage = MB86H_NATIVE_FAILED;
        result->download.status = USB_ARGUMENT;
        return USB_ARGUMENT;
    }
    pthread_mutex_lock(&device->control_mutex);
    pthread_mutex_lock(&device->streams_mutex);
    if (atomic_load(&device->native_taken_over)) { rc = -EALREADY; goto done; }
    for (i = 0; i < USB_ENDPOINT_SLOTS; ++i) {
        pthread_mutex_lock(&device->streams[i].mutex);
        if (device->streams[i].submitted || device->streams[i].in_callback) rc = -EBUSY;
        pthread_mutex_unlock(&device->streams[i].mutex);
    }
    if (rc) goto done;
    device->download_options = options;
    rc = usb_boot_download(device->info->usb_boot->download, &native_boot_io, device,
                            image, size, options, result);
    device->download_options = NULL;
    if (result->download.recovery_required || result->download.stage == MB86H_NATIVE_DONE)
        atomic_store(&device->native_taken_over, true);
done:
    if (rc && !result->download.status) {
        result->download.stage = MB86H_NATIVE_FAILED;
        result->download.status = rc;
    }
    pthread_mutex_unlock(&device->streams_mutex);
    pthread_mutex_unlock(&device->control_mutex);
    return rc;
}

static int device_boot(struct usb_device *device, struct usb_boot_result *result,
                        bool prepare)
{
    int rc;
    if (!device || !result || !device->info || !device->info->usb_boot ||
        !(prepare ? device->info->usb_boot->prepare : device->info->usb_boot->observe) ||
        pthread_equal(pthread_self(), device->event_thread)) return USB_ARGUMENT;
    pthread_mutex_lock(&device->control_mutex);
    if (prepare && atomic_load(&device->native_taken_over)) {
        pthread_mutex_unlock(&device->control_mutex);
        return -EALREADY;
    }
    device->receive_status = (struct usb_libusb_receive_status){.transfer_status = -1};
    rc = prepare ? device->info->usb_boot->prepare(&native_boot_io, device, result) :
                   device->info->usb_boot->observe(&native_boot_io, device, result);
    pthread_mutex_unlock(&device->control_mutex);
    return native_result(rc);
}

int usb_libusb_prepare_boot0(struct usb_device *device, struct usb_boot_result *result)
{
    return device_boot(device, result, true);
}

int usb_libusb_observe_boot0(struct usb_device *device, struct usb_boot_result *result)
{
    return device_boot(device, result, false);
}

int usb_libusb_boot0_read(struct usb_device *device,uint32_t address,void *data,size_t size)
{
    if (!device || !data || !size)
        return USB_ARGUMENT;
    if (device->info && device->info->usb_boot && device->info->usb_boot->read) {
        pthread_mutex_lock(&device->control_mutex);
        int rc = native_result(device->info->usb_boot->read(&native_boot_io, device, address, data, size));
        pthread_mutex_unlock(&device->control_mutex);
        return rc;
    }
    if (!is_torne(device)) {
        pthread_mutex_lock(&device->control_mutex);
        int rc = native_result(usb_xz_window(&native_ops, device, address, data, size, false));
        pthread_mutex_unlock(&device->control_mutex);
        return rc;
    }
    /* Sony's native BC handler reaches the low 24-bit address space.  The
     * resident reservation is deliberately the sole high-address exception. */
    pthread_mutex_lock(&device->control_mutex);
    int rc = native_result(usb_native_window(&native_ops, device, native_window_profile(device), address, data, size, false, false));
    pthread_mutex_unlock(&device->control_mutex);
    return rc;
}
int usb_libusb_boot0_write(struct usb_device *device,uint32_t address,const void *data,size_t size)
{
    if (!device || !data || !size || (address & 3U) || (size & 3U))
        return USB_ARGUMENT;
    if (native_boot_only(device) && !native_window_profile(device)) return USB_ARGUMENT;
    if (!native_window_profile(device)) {
        pthread_mutex_lock(&device->control_mutex);
        int rc = native_result(usb_xz_window(&native_ops, device, address, (void *)data, size, true));
        pthread_mutex_unlock(&device->control_mutex);
        return rc;
    }
    pthread_mutex_lock(&device->control_mutex);
    int rc = native_result(usb_native_window(&native_ops, device, native_window_profile(device), address, (void *)data,
                                             size, true, device->torne_block_writes));
    pthread_mutex_unlock(&device->control_mutex);
    return rc;
}
