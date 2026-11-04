/* SPDX-License-Identifier: MIT */
#include "mb86H/card.h"
#include "mb86H/protocol.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

void mb86H_card_init(struct mb86H_card *c,
                      const struct mb86H_card_ops *ops, void *opaque)
{
    *c = (struct mb86H_card){ .ops = ops, .opaque = opaque };
}

/* Keep the original firmware result for diagnostics, map only at this API. */
static int result(int rc)
{
    switch (rc) {
    case -2600: return -EBUSY;
    case -2601: return -EOPNOTSUPP;
    case -2602: return -EIO;
    case -2603: return -ETIMEDOUT;
    case -2604: return -EMSGSIZE;
    default: return rc;
    }
}

static int command(struct mb86H_card *c, unsigned op,
                   uint8_t data[40], uint8_t response[64])
{
    int rc;
    if (c->failed) return c->failed;
    memset(response, 0, 64);
    data[0] = MB86H_CARD_VERSION;
    mb86H_debug_u32be_put(data + 4, c->generation);
    rc = c->ops->command(c->opaque, op, data, response);
    /* Transport failure is terminal: the command may have executed. */
    if (rc) return c->failed = rc;
    rc = (int)mb86H_debug_u32be_get(response + 20);
    c->firmware_result = rc;
    /* A dispatcher rejection precedes the v2 response format. */
    if (mb86H_debug_u32be_get(response + 8) != MB86H_DEBUG_OK && rc < 0)
        return result(rc);
    if (response[24] != MB86H_CARD_VERSION || rc > 1 ||
        mb86H_debug_u32be_get(response + 8) != MB86H_DEBUG_OK)
        return c->failed = -EPROTO;
    if ((op == MB86H_CARD_POWER_ON || op == MB86H_CARD_RESET) && rc >= 0)
        c->generation = mb86H_debug_u32be_get(response + 16);
    else if (op != MB86H_CARD_STATUS && op != MB86H_CARD_POWER_OFF &&
             mb86H_debug_u32be_get(response + 16) != c->generation)
        return c->failed = -EPROTO;
    return result(rc);
}

static int settle(struct mb86H_card *c, int rc, uint8_t response[64])
{
    uint32_t start = c->ops->now_ms(c->opaque);
    while (rc == 1) {
        uint8_t data[40] = {0};
        if ((uint32_t)(c->ops->now_ms(c->opaque) - start) >= 5000)
            return c->failed = -ETIMEDOUT;
        /* No control mutex is held while sleeping: RF service must run. */
        c->ops->delay_ms(c->opaque, 10);
        rc = command(c, MB86H_CARD_SERVICE, data, response);
    }
    return rc;
}

int mb86H_card_status(struct mb86H_card *c, uint8_t *icc)
{
    uint8_t data[40] = {0}, response[64];
    unsigned state;
    int rc;
    if (!icc) return -EINVAL;
    rc = command(c, MB86H_CARD_STATUS, data, response);
    if (rc) return rc;
    state = mb86H_debug_u32be_get(response + 12);
    if (!response[39]) {
        c->active = false; c->atr_length = 0;
        memset(&c->parameters, 0, sizeof c->parameters);
        if (response[38]) { rc = mb86H_card_power_off(c); if (rc) return rc; }
        *icc = MB86H_CARD_ABSENT;
    } else {
        *icc = state >= 2 && state <= 6 ? MB86H_CARD_ACTIVE : MB86H_CARD_INACTIVE;
        if (*icc != MB86H_CARD_ACTIVE) { c->active = false; c->atr_length = 0; }
    }
    return 0;
}

int mb86H_card_power_off(struct mb86H_card *c)
{
    uint8_t data[40] = {0}, response[64];
    int rc = settle(c, command(c, MB86H_CARD_POWER_OFF, data, response), response);
    if (!rc) { c->active = false; c->atr_length = 0; memset(&c->parameters, 0, sizeof c->parameters); }
    return rc;
}

static int read_bytes(struct mb86H_card *c, unsigned op,
                      uint8_t *out, size_t *length, unsigned limit)
{
    unsigned at = 0, total = 0;
    size_t capacity = *length;
    *length = 0;
    do {
        uint8_t data[40] = {0}, response[64];
        unsigned n, reported;
        int rc;
        data[1] = MB86H_CARD_CHUNK;
        data[2] = (uint8_t)(at >> 8); data[3] = (uint8_t)at;
        rc = command(c, op, data, response);
        if (rc) return rc;
        reported = ((unsigned)response[26] << 8) | response[27];
        n = response[25];
        if (reported > limit || (at && reported != total) || !n ||
            n > MB86H_CARD_CHUNK || at + n > reported)
            return c->failed = -EPROTO;
        total = reported;
        if (total > capacity) { *length = total; return -EMSGSIZE; }
        memcpy(out + at, response + 32, n);
        at += n;
    } while (at < total);
    *length = total;
    return 0;
}

static int activate(struct mb86H_card *c,unsigned op,uint8_t *atr,size_t *length)
{
    uint8_t data[40] = {0}, response[64];
    int rc;
    if (!atr || !length) return -EINVAL;
    if (c->failed) { *length=0; return c->failed; }
    if (*length < MB86H_CARD_MAX_ATR) { *length = MB86H_CARD_MAX_ATR; return -EMSGSIZE; }
    if (op == MB86H_CARD_POWER_ON && c->active && c->atr_length) {
        memcpy(atr,c->atr,c->atr_length);*length=c->atr_length;return 0;
    }
    if (op == MB86H_CARD_RESET && !c->active) op=MB86H_CARD_POWER_ON;
    c->atr_length=0;c->active=false;
    rc=settle(c,command(c,op,data,response),response);
    if (!rc) {
        c->atr_length=sizeof c->atr;
        rc=read_bytes(c,MB86H_CARD_ATR_READ,c->atr,&c->atr_length,MB86H_CARD_MAX_ATR);
    }
    if (!rc) rc=mb86H_card_parse_atr(c->atr,c->atr_length,&c->parameters);
    if (rc) { c->atr_length=0;*length=0;return rc; }
    c->active=true;*length=c->atr_length;memcpy(atr,c->atr,c->atr_length);return 0;
}
int mb86H_card_power_on(struct mb86H_card *c,uint8_t *atr,size_t *length)
{return activate(c,MB86H_CARD_POWER_ON,atr,length);}
int mb86H_card_reset(struct mb86H_card *c,uint8_t *atr,size_t *length)
{return activate(c,MB86H_CARD_RESET,atr,length);}

int mb86H_card_transfer(struct mb86H_card *c, const uint8_t *tx,
                          size_t tx_length, uint8_t *rx, size_t *rx_length)
{
    uint8_t data[40] = {0}, response[64];
    unsigned at;
    int rc;
    if (!tx || !tx_length || tx_length > MB86H_CARD_MAX_TX || !rx || !rx_length)
        return -EINVAL;
    if (!c->active) return -EIO;
    if (c->parameters.protocol > 1) return -EOPNOTSUPP;
    /* A short caller buffer must not execute an APDU which it cannot receive. */
    if (*rx_length < MB86H_CARD_MAX_RX) {
        *rx_length = MB86H_CARD_MAX_RX;
        return -EMSGSIZE;
    }
    data[2] = (uint8_t)(tx_length >> 8); data[3] = (uint8_t)tx_length;
    mb86H_debug_u32be_put(data + 8, 3000);
    rc = command(c, MB86H_CARD_TX_BEGIN, data, response);
    for (at = 0; !rc && at < tx_length;) {
        unsigned n = (unsigned)tx_length - at;
        if (n > MB86H_CARD_CHUNK) n = MB86H_CARD_CHUNK;
        memset(data, 0, sizeof(data));
        data[1] = (uint8_t)n; data[2] = (uint8_t)(at >> 8); data[3] = (uint8_t)at;
        memcpy(data + 8, tx + at, n);
        rc = command(c, MB86H_CARD_TX_DATA, data, response);
        at += n;
    }
    memset(data, 0, sizeof(data));
    if (!rc) rc = settle(c, command(c, MB86H_CARD_TRANSMIT, data, response), response);
    if (!rc) rc = read_bytes(c, MB86H_CARD_RX_READ, rx, rx_length, MB86H_CARD_MAX_RX);
    if (rc) { c->active = false; *rx_length = 0; }
    return rc;
}
