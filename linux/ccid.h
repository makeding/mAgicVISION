#ifndef MB86H_CCID_H
#define MB86H_CCID_H

#include <linux/types.h>
#include <linux/module.h>
#include <linux/errno.h>
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

/* A successful registration holds the optional CCID module until teardown. */
static inline int mb86H_ccid_try_register(const struct mb86H_ccid_ops *ops,
        void *opaque, const char *product, const char *serial)
{
    int (*register_reader)(const struct mb86H_ccid_ops *, void *, const char *, const char *);
    int rc;

    register_reader = symbol_get(mb86H_ccid_register);
    if (!register_reader) return -ENODEV;
    rc = register_reader(ops, opaque, product, serial);
    if (rc)
        symbol_put(mb86H_ccid_register);
    return rc;
}

static inline void mb86H_ccid_release(void *opaque)
{
    void (*unregister_reader)(void *) = symbol_get(mb86H_ccid_unregister);
    if (unregister_reader) {
        unregister_reader(opaque);
        symbol_put(mb86H_ccid_unregister);
    }
    symbol_put(mb86H_ccid_register);
}

/* Provision dummy_hcd for simultaneous readers. Optional reader admission
 * failures are independent of DVB initialization and resume. Module lifetime
 * and the shared registry belong exclusively to mb86H_ccid. */

#endif
