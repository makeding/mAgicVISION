/* SPDX-License-Identifier: MIT */
#include "mb86H/resident_image.h"
#include "mb86H/protocol/constants.h"
#include "mb86H/bytes.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

static uint32_t resident_offset(uint32_t address)
{
    return address - MB86H_RESIDENT_BASE;
}
static void observe(const struct mb86H_resident_image_ops *ops,void *opaque,
                    enum mb86H_loader_stage stage,int began,int status)
{ if(ops && ops->observe) ops->observe(opaque,&(struct mb86H_loader_observation){stage,began,status}); }

int mb86H_resident_image_validate(const uint8_t *image)
{
    const uint8_t *meta;
    uint32_t offset, sum = 0;

    if (!image) return -EINVAL;
    meta = image + resident_offset(MB86H_RESIDENT_META);
    if (mb86H_be32_get(meta) != MB86H_RESIDENT_HEADER_MAGIC ||
        mb86H_be32_get(meta + 4) != MB86H_MIN_SUPPORTED_ABI ||
        mb86H_be32_get(meta + 8) != MB86H_RESIDENT_BYTES ||
        mb86H_be32_get(meta + 12) != MB86H_RESIDENT_BASE ||
        mb86H_be32_get(meta + 16) != MB86H_RESIDENT_IMMUTABLE_END ||
        mb86H_be32_get(meta + 20) != MB86H_RESIDENT_IMMUTABLE_CHECKSUM ||
        mb86H_be32_get(image + resident_offset(MB86H_RESIDENT_COMMIT)) !=
            MB86H_RESIDENT_COMMIT_MAGIC)
        return -EINVAL;
    for (offset = 0;
         offset < resident_offset(MB86H_RESIDENT_IMMUTABLE_END);
         offset += 4)
        sum += mb86H_be32_get(image + offset);
    return sum ? -EBADMSG : 0;
}

int mb86H_resident_image_block(const struct mb86H_resident_image_ops *ops,
                                  void *opaque, uint32_t offset, bool write,
                                  const uint8_t *src, uint8_t *dst,
                                  uint32_t length)
{
    uint8_t request[40] = {0}, response[64];
    int rc;

    if (!ops || !ops->session || !ops->session_ops ||
        !ops->session_ops->exchange || !length ||
        length > MB86H_RESIDENT_BLOCK_BYTES || (offset & 3) ||
        (length & 3) || offset > MB86H_RESIDENT_BYTES - length ||
        (write && !src) || (!write && !dst))
        return -EINVAL;
    if (write)
        memcpy(request, src, length);
    rc = mb86H_session_exchange(ops->session, ops->session_ops, opaque,
                                   MB86H_DEBUG_RESIDENT_BLOCK, offset,
                                   write ? 1 : 0, length, request, response);
    if (rc || mb86H_be32_get(response + MB86H_DEBUG_PACKET_OPERATION_OFFSET) !=
                  MB86H_DEBUG_OK ||
        mb86H_be32_get(response + 12) != length || mb86H_be32_get(response + 16) != offset)
        return rc ? rc : -EIO;
    if (!write)
        memcpy(dst, response + MB86H_DEBUG_PACKET_DATA_OFFSET, length);
    return 0;
}

int mb86H_resident_image_verify(const struct mb86H_resident_image_ops *ops,
                                   void *opaque, const uint8_t *image,
                                   bool skip_commit)
{
    uint8_t got[MB86H_RESIDENT_BLOCK_BYTES];
    uint32_t offset, i, commit = resident_offset(MB86H_RESIDENT_COMMIT);
    int rc;

    observe(ops,opaque,MB86H_LOADER_READBACK,1,0);
    if (!image) { observe(ops,opaque,MB86H_LOADER_READBACK,0,-EINVAL); return -EINVAL; }
    for (offset = 0; offset < MB86H_RESIDENT_BYTES; offset += sizeof(got)) {
        uint32_t length = MB86H_RESIDENT_BYTES - offset;
        if (length > sizeof(got))
            length = sizeof(got);
        rc = mb86H_resident_image_block(ops, opaque, offset, false, 0, got,
                                           length);
        if (rc)
            { observe(ops,opaque,MB86H_LOADER_READBACK,0,rc); return rc; }
        for (i = 0; i < length; ++i)
            if ((!skip_commit || offset + i < commit || offset + i >= commit + 4) &&
                got[i] != image[offset + i]) {
                if (ops->mismatch)
                    ops->mismatch(opaque, MB86H_RESIDENT_BASE + offset + i,
                                  image[offset + i], got[i]);
                { observe(ops,opaque,MB86H_LOADER_READBACK,0,-EIO); return -EIO; }
            }
    }
    observe(ops,opaque,MB86H_LOADER_READBACK,0,0); return 0;
}

int mb86H_resident_image_program(const struct mb86H_resident_image_ops *ops,
                                    void *opaque, const uint8_t *image)
{
    uint8_t zero[4] = {0}, commit[4];
    uint32_t offset, commit_offset = resident_offset(MB86H_RESIDENT_COMMIT);
    int rc;

    observe(ops,opaque,MB86H_LOADER_WRITE,1,0);
    rc = mb86H_resident_image_validate(image);
    if (rc) { observe(ops,opaque,MB86H_LOADER_WRITE,0,rc); return rc; }
    rc = mb86H_resident_image_block(ops, opaque, commit_offset, true, zero,
                                       0, sizeof(zero));
    for (offset = 0; !rc && offset < MB86H_RESIDENT_BYTES;
         offset += MB86H_RESIDENT_BLOCK_BYTES) {
        uint32_t length = MB86H_RESIDENT_BYTES - offset;
        if (length > MB86H_RESIDENT_BLOCK_BYTES)
            length = MB86H_RESIDENT_BLOCK_BYTES;
        if (offset <= commit_offset && commit_offset < offset + length) {
            uint32_t before = commit_offset - offset, after = before + 4;
            if (before)
                rc = mb86H_resident_image_block(ops, opaque, offset, true,
                                                   image + offset, 0, before);
            if (!rc && after < length)
                rc = mb86H_resident_image_block(ops, opaque, offset + after,
                                                   true, image + offset + after, 0,
                                                   length - after);
        } else {
            rc = mb86H_resident_image_block(ops, opaque, offset, true,
                                               image + offset, 0, length);
        }
    }
    if (rc) { observe(ops,opaque,MB86H_LOADER_WRITE,0,rc); return rc; }
    observe(ops,opaque,MB86H_LOADER_WRITE,0,0);
    observe(ops,opaque,MB86H_LOADER_COMMIT,1,0);
    rc = mb86H_resident_image_block(ops, opaque, commit_offset, true,
                                       image + commit_offset, 0, sizeof(commit));
    if (rc) { observe(ops,opaque,MB86H_LOADER_COMMIT,0,rc); return rc; }
    rc = mb86H_resident_image_block(ops, opaque, commit_offset, false, 0,
                                       commit, sizeof(commit));
    rc=rc ? rc : (memcmp(commit,image+commit_offset,sizeof(commit))?-EIO:0);
    observe(ops,opaque,MB86H_LOADER_COMMIT,0,rc); return rc;
}
