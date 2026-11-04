#include "libusb_internal.h"

#ifdef USB_LIBUSB_TESTING
int (*test_submit_transfer)(struct libusb_transfer *transfer);
int (*test_cancel_transfer)(struct libusb_transfer *transfer);
int (*test_endpoint_type)(uint8_t endpoint, uint8_t *type);
int (*test_clear_halt)(uint8_t endpoint);
#endif

int submit_transfer(struct libusb_transfer *transfer)
{
#ifdef USB_LIBUSB_TESTING
    if (test_submit_transfer)
        return test_submit_transfer(transfer);
#endif
    return libusb_submit_transfer(transfer);
}

int cancel_transfer(struct libusb_transfer *transfer)
{
#ifdef USB_LIBUSB_TESTING
    if (test_cancel_transfer)
        return test_cancel_transfer(transfer);
#endif
    return libusb_cancel_transfer(transfer);
}
void control_done(struct libusb_transfer *transfer)
{
    struct control_wait *wait = transfer->user_data;
    pthread_mutex_lock(&wait->mutex);
    wait->status = transfer->status;
    wait->actual = transfer->actual_length;
    wait->done = 1;
    pthread_cond_signal(&wait->complete);
    pthread_mutex_unlock(&wait->mutex);
}
int control_locked(struct usb_device *device,
                          struct usb_control_step *steps, size_t step_count,
                          unsigned timeout_ms)
{
    size_t index;
    if (!device || !steps || !step_count)
        return USB_ARGUMENT;
    if (pthread_equal(pthread_self(), device->event_thread))
        return USB_CLOSED;
    for (index = 0; index < step_count; ++index) {
        steps[index].actual_length = 0;
        steps[index].submit_result = 0;
        steps[index].transfer_status = -1;
        steps[index].timeout_ms = timeout_ms;
    }
    for (index = 0; index < step_count; ++index) {
        if (steps[index].length && !steps[index].data)
            return USB_ARGUMENT;
    }
    for (index = 0; index < step_count; ++index) {
        struct control_wait wait = {0};
        struct libusb_transfer *transfer;
        unsigned char *buffer;
        int actual;

        transfer = libusb_alloc_transfer(0);
        buffer = calloc(1, LIBUSB_CONTROL_SETUP_SIZE + steps[index].length);
        if (!transfer || !buffer) {
            libusb_free_transfer(transfer);
            free(buffer);
            return USB_TRANSFER;
        }
        libusb_fill_control_setup(buffer, steps[index].request_type, steps[index].request,
                                  steps[index].value, steps[index].index, steps[index].length);
        if (!(steps[index].request_type & LIBUSB_ENDPOINT_IN) && steps[index].length)
            memcpy(buffer + LIBUSB_CONTROL_SETUP_SIZE, steps[index].data, steps[index].length);
        if (pthread_mutex_init(&wait.mutex, NULL) != 0) {
            libusb_free_transfer(transfer);
            free(buffer);
            return USB_TRANSFER;
        }
        if (pthread_cond_init(&wait.complete, NULL) != 0) {
            pthread_mutex_destroy(&wait.mutex);
            libusb_free_transfer(transfer);
            free(buffer);
            return USB_TRANSFER;
        }
        libusb_fill_control_transfer(transfer, device->handle, buffer, control_done, &wait,
                                     timeout_ms);
        steps[index].submit_result = submit_transfer(transfer);
        if (steps[index].submit_result != 0) {
            if (device->observer)
                device->observer(device->observer_opaque, &steps[index], NULL);
            pthread_cond_destroy(&wait.complete);
            pthread_mutex_destroy(&wait.mutex);
            libusb_free_transfer(transfer);
            free(buffer);
            return USB_TRANSFER;
        }
        pthread_mutex_lock(&wait.mutex);
        while (!wait.done)
            pthread_cond_wait(&wait.complete, &wait.mutex);
        actual = wait.actual;
        steps[index].transfer_status = (int)wait.status;
        pthread_mutex_unlock(&wait.mutex);
        if (actual >= 0 && actual <= steps[index].length)
            steps[index].actual_length = (uint16_t)actual;
        if (wait.status == LIBUSB_TRANSFER_COMPLETED && actual <= steps[index].length &&
            (steps[index].request_type & LIBUSB_ENDPOINT_IN) && actual > 0)
            memcpy(steps[index].data, buffer + LIBUSB_CONTROL_SETUP_SIZE, (size_t)actual);
        if (device->observer)
            device->observer(device->observer_opaque, &steps[index],
                             buffer + LIBUSB_CONTROL_SETUP_SIZE);
        pthread_cond_destroy(&wait.complete);
        pthread_mutex_destroy(&wait.mutex);
        libusb_free_transfer(transfer);
        free(buffer);
        if (wait.status != LIBUSB_TRANSFER_COMPLETED || actual < 0 ||
            actual > steps[index].length ||
            (!(steps[index].request_type & LIBUSB_ENDPOINT_IN) &&
                           actual != steps[index].length)) {
            return USB_TRANSFER;
        }
    }
    return USB_OK;
}

int usb_libusb_control(struct usb_device *device,
                       struct usb_control_step *steps, size_t step_count,
                       unsigned timeout_ms)
{
    int rc;
    if (!device || !steps || !step_count)
        return USB_ARGUMENT;
    pthread_mutex_lock(&device->control_mutex);
    rc = control_locked(device, steps, step_count, timeout_ms);
    pthread_mutex_unlock(&device->control_mutex);
    return rc;
}

static int window_transfer(struct usb_device *device, uint32_t address,
                           uint8_t *data, size_t size,
                           enum usb_window_direction direction,
                           unsigned timeout_ms)
{
    size_t offset = 0;
    if (!device || !data || !size || size > USB_WINDOW_MAX_TRANSFER)
        return USB_ARGUMENT;
    while (offset < size) {
        size_t chunk = size - offset;
        struct usb_window_control control;
        struct usb_control_step step;
        if (chunk > USB_WINDOW_CONTROL_CHUNK)
            chunk = USB_WINDOW_CONTROL_CHUNK;
        if (usb_window_control_from_address(address + (uint32_t)offset, chunk,
                                             direction, &control) != 0)
            return USB_ARGUMENT;
        step = (struct usb_control_step){
            .request_type = control.request_type,
            .request = control.request,
            .value = control.value,
            .index = control.index,
            .data = data + offset,
            .length = control.length,
        };
        if (usb_libusb_control(device, &step, 1, timeout_ms) != USB_OK)
            return USB_TRANSFER;
        if (direction == USB_WINDOW_IN && step.actual_length != chunk)
            return USB_TRANSFER;
        offset += chunk;
    }
    return USB_OK;
}

int usb_libusb_window_write(struct usb_device *device, uint32_t address,
                            const uint8_t *data, size_t size,
                            unsigned timeout_ms)
{
    if (device && native_boot_only(device)) return USB_ARGUMENT;
    return window_transfer(device, address, (uint8_t *)data, size,
                           USB_WINDOW_OUT, timeout_ms);
}

int usb_libusb_window_read(struct usb_device *device, uint32_t address,
                           uint8_t *data, size_t size,
                           unsigned timeout_ms)
{
    return window_transfer(device, address, data, size, USB_WINDOW_IN,
                           timeout_ms);
}
