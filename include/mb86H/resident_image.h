/* SPDX-License-Identifier: MIT */
/* Transport-neutral resident image upload protocol. */
#ifndef MB86H_RESIDENT_IMAGE_H
#define MB86H_RESIDENT_IMAGE_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 uint8_t;
typedef u32 uint32_t;
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#include "mb86H/session.h"

enum mb86H_loader_stage { MB86H_LOADER_ADOPT_VALIDATE, MB86H_LOADER_HOOK_SNAPSHOT,
    MB86H_LOADER_RESIDENT_SNAPSHOT, MB86H_LOADER_WRITE, MB86H_LOADER_READBACK,
    MB86H_LOADER_COMMIT, MB86H_LOADER_CAPS, MB86H_LOADER_PREPARE,
    MB86H_LOADER_INITIALIZE, MB86H_LOADER_RESTORE, MB86H_LOADER_STAGE_COUNT };
struct mb86H_loader_observation { enum mb86H_loader_stage stage; uint8_t began; int status; };
typedef void (*mb86H_loader_observe)(void *,const struct mb86H_loader_observation *);

struct mb86H_resident_image_ops {
    struct mb86H_session *session;
    const struct mb86H_session_ops *session_ops;
    void (*mismatch)(void *opaque, uint32_t address,
                     uint32_t expected, uint32_t observed);
    mb86H_loader_observe observe;
};

int mb86H_resident_image_validate(const uint8_t *image);
int mb86H_resident_image_block(const struct mb86H_resident_image_ops *ops,
                                  void *opaque, uint32_t offset, bool write,
                                  const uint8_t *src, uint8_t *dst,
                                  uint32_t length);
int mb86H_resident_image_verify(const struct mb86H_resident_image_ops *ops,
                                   void *opaque, const uint8_t *image,
                                   bool skip_commit);
/* The caller owns lifecycle state and must mark the image callable on success. */
int mb86H_resident_image_program(const struct mb86H_resident_image_ops *ops,
                                    void *opaque, const uint8_t *image);

#endif
