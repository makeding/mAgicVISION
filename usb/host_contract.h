/* SPDX-License-Identifier: MIT */
/* Private host-side transport facts.  These are USB plumbing, not firmware
 * control: channel selection and device state remain semantic FW commands. */
#ifndef USB_HOST_CONTRACT_H
#define USB_HOST_CONTRACT_H

#include <stdint.h>
#include "mb86H/protocol/constants.h"

enum {
    USB_TS_ENDPOINT = USB_STREAM_ENDPOINT,
    USB_AUX_ENDPOINT = USB_DIAGNOSTIC_ENDPOINT,
    USB_ENDPOINT_SLOTS = 2, /* stream and diagnostic endpoints, not two tuners */
    USB_RECEIVE_BUFFER_SIZE = 16384,
};

static inline int usb_endpoint_slot(uint8_t endpoint, unsigned *slot)
{
    if (!slot)
        return 0;
    switch (endpoint) {
    case USB_TS_ENDPOINT:
        *slot = 0;
        return 1;
    case USB_AUX_ENDPOINT:
        *slot = 1;
        return 1;
    default:
        return 0;
    }
}

#endif
