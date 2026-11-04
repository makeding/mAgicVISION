/* MSVC x64 BonDriver binary interface. Slot order is part of the ABI.
 * Reference: DBCTRADO/LibISDB Windows/Base/BonDriver.hpp.
 * GCC C++ virtual tables/RTTI are intentionally not exposed to TVTest. */
#ifndef MB86H_BON_ABI_H
#define MB86H_BON_ABI_H
#include "host.h"
#if !defined(_WIN64) || !defined(__x86_64__)
#error This BonDriver ABI is Windows x64 only
#endif
struct bon;
struct bon_vtable {
    BOOL (*open)(struct bon *);
    void (*close)(struct bon *);
    BOOL (*channel_byte)(struct bon *,BYTE);
    float (*signal)(struct bon *);
    DWORD (*wait)(struct bon *,DWORD);
    DWORD (*ready)(struct bon *);
    BOOL (*stream_pointer)(struct bon *,BYTE **,DWORD *,DWORD *);
    BOOL (*stream_copy)(struct bon *,BYTE *,DWORD *,DWORD *);
    void (*purge)(struct bon *);
    void (*release)(struct bon *);
    const wchar_t *(*name)(struct bon *);
    BOOL (*opening)(struct bon *);
    const wchar_t *(*space)(struct bon *,DWORD);
    const wchar_t *(*channel_name)(struct bon *,DWORD,DWORD);
    BOOL (*channel)(struct bon *,DWORD,DWORD);
    DWORD (*current_space)(struct bon *);
    DWORD (*current_channel)(struct bon *);
};
const struct bon_vtable *bon_msvc_table(const struct bon_vtable *);
#endif
