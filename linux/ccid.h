#ifndef MB86H_CCID_H
#define MB86H_CCID_H

#include <linux/types.h>
#include "mb86H/card.h"

/*
 * The shared CCID module is a frontend of each physical backend. The USB
 * or PCIe owner supplies semantic operations while holding its own loader
 * and transport lifetime.  All callbacks run in the CCID worker, never from
 * a gadget endpoint completion callback.  The length arguments are input
 * capacities and output actual lengths.
 */
struct mb86H_ccid_ops {
	int (*status)(void *opaque, u8 *icc_status);
	int (*power_on)(void *opaque, u8 *atr, size_t *length);
	int (*power_off)(void *opaque);
	int (*transfer)(void *opaque, const u8 *tx, size_t tx_len,
			u8 *rx, size_t *rx_len);
};

/* One independent reader/controller per physical backend. Product and serial
 * are copied and remain valid through composite unbind. */
int mb86H_ccid_register(const struct mb86H_ccid_ops *ops, void *opaque,
                        const char *product, const char *serial);
void mb86H_ccid_unregister(void *opaque);

/* Provision dummy_hcd for simultaneous readers. Optional reader admission
 * failures are independent of DVB initialization and resume. Module lifetime
 * and the shared registry belong exclusively to mb86H_ccid. */

#endif
