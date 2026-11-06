#include "mb86H/card.h"
#include "mb86H/protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fake {
    unsigned calls, power, transmit, upload, now, state;
    int transport, rejected, malformed, generation_mismatch, absent;
};
static uint32_t now(void *p) { return ((struct fake *)p)->now; }
static void delay(void *p, unsigned ms) { ((struct fake *)p)->now += ms; }
static int command(void *p, unsigned op, const uint8_t data[40], uint8_t rx[64])
{
    struct fake *f = p;
    unsigned offset = (unsigned)data[1] << 8 | data[2];
    ++f->calls;
    if (f->transport) return f->transport;
    assert(!data[3]);
    memset(rx, 0, 64);
    mb86H_debug_u32be_put(rx + 8, MB86H_DEBUG_OK);
    mb86H_debug_u32be_put(rx + 16, f->generation_mismatch ? 8 : 7);
    rx[39] = !f->absent;rx[38] = f->state != 0;
    if (f->rejected) {
        mb86H_debug_u32be_put(rx + 8, MB86H_DEBUG_FATAL);
        mb86H_debug_u32be_put(rx + 20, (uint32_t)f->rejected);
        rx[24] = 0; /* Runtime rejects before card dispatcher. */
        return 0;
    }
    switch (op) {
    case MB86H_CARD_RESET:
    case MB86H_CARD_POWER_ON:
        ++f->power; f->state = 1;
        mb86H_debug_u32be_put(rx + 20, 1);
        break;
    case MB86H_CARD_SERVICE: f->state = 2; break;
    case MB86H_CARD_STATUS: break;
    case MB86H_CARD_ATR_READ:
        rx[24] = f->malformed ? 34 : 2; rx[26] = 2;
        rx[32] = 0x3b; rx[33] = 0;
        break;
    case MB86H_CARD_POWER_OFF: f->state = 0; break;
    case MB86H_CARD_TX_BEGIN: f->upload = 0; break;
    case MB86H_CARD_TX_DATA:
        assert(offset == f->upload && data[0] && data[0] <= 32);
        f->upload += data[0]; break;
    case MB86H_CARD_TRANSMIT: ++f->transmit; f->state = 6; break;
    case MB86H_CARD_RX_READ:
        rx[24] = 2; rx[26] = 2; rx[32] = 0x90; rx[33] = 0; break;
    default: assert(0);
    }
    mb86H_debug_u32be_put(rx + 12, f->state);
    return 0;
}
static const struct mb86H_card_ops ops = {command, now, delay};
int main(void)
{
    struct mb86H_card card;
    struct fake f = {0};
    uint8_t atr[33], tx[256] = {0}, rx[512], status;
    size_t n;
    unsigned calls;
    {
        uint8_t sample[] = {0x3b,0xf0,0x12,0,0xff,0x91,0x81,0xb1,0x7c,0x45,0x1f,3,0x99};
        struct mb86H_card_parameters params;
        assert(!mb86H_card_parse_atr(sample,sizeof sample,&params));
        assert(params.protocol==1&&params.fi_di==0x12&&params.guard==255&&params.ifsc==124&&params.bwi==4&&params.cwi==5);
        assert(mb86H_card_parse_atr(sample,8,&params)==-EPROTO);
        sample[2]=0x33;sample[12]^=0x12^0x33;
        assert(!mb86H_card_parse_atr(sample,sizeof sample,&params)&&params.fi_di==0x33);
        sample[12]^=1;assert(mb86H_card_parse_atr(sample,sizeof sample,&params)==-EPROTO);
    }
    mb86H_card_init(&card, &ops, &f);
    /* Semantic presence is reported by FW, not decoded from raw MMIO. */
    assert(!mb86H_card_status(&card,&status)&&status==MB86H_CARD_INACTIVE);
    n = 1; calls = f.calls;
    assert(mb86H_card_power_on(&card, atr, &n) == -EMSGSIZE);
    assert(n == 33 && f.calls == calls);
    n = sizeof atr;
    assert(!mb86H_card_power_on(&card, atr, &n));
    assert(n == 2 && atr[0] == 0x3b && card.generation == 7 && f.power == 1);
    assert(f.now == 10 && card.active);
    calls=f.calls;n=sizeof atr;
    assert(!mb86H_card_power_on(&card,atr,&n)&&f.calls==calls);
    n=sizeof atr;assert(!mb86H_card_reset(&card,atr,&n)&&f.power==2);
    f.absent=1;assert(!mb86H_card_status(&card,&status)&&status==MB86H_CARD_ABSENT&&!card.atr_length&&!card.active);
    f.absent=0;n=sizeof atr;assert(!mb86H_card_power_on(&card,atr,&n));
    assert(!mb86H_card_status(&card, &status) && status == 0);
    n = 1; calls = f.calls;
    assert(mb86H_card_transfer(&card, tx, sizeof tx, rx, &n) == -EMSGSIZE);
    assert(f.calls == calls && !f.transmit);
    n = sizeof rx;
    assert(!mb86H_card_transfer(&card, tx, sizeof tx, rx, &n));
    assert(n == 2 && rx[0] == 0x90 && f.upload == 256 && f.transmit == 1);
    assert(!mb86H_card_power_off(&card) && !card.active);

    /* Ambiguous transport failure is terminal, including power/reset calls. */
    f.transport = -EPIPE; n = sizeof atr;
    assert(mb86H_card_power_on(&card, atr, &n) == -EPIPE);
    calls = f.calls; f.transport = 0;
    assert(mb86H_card_power_off(&card) == -EPIPE && f.calls == calls);

    mb86H_card_init(&card, &ops, &f); f.rejected = MB86H_FE_INVALID; n = sizeof atr;
    assert(mb86H_card_power_on(&card, atr, &n) == -EINVAL);
    assert(!card.failed && card.firmware_result == MB86H_FE_INVALID);
    mb86H_card_init(&card, &ops, &f); f.rejected = MB86H_FE_BUSY; n = sizeof atr;
    assert(mb86H_card_power_on(&card, atr, &n) == -EBUSY);
    assert(!card.failed && card.firmware_result == MB86H_FE_BUSY);
    f.rejected = 0; f.malformed = 1; n = sizeof atr;
    assert(mb86H_card_power_on(&card, atr, &n) == -EPROTO);
    calls = f.calls;
    assert(mb86H_card_status(&card, &status) == -EPROTO && f.calls == calls);

    mb86H_card_init(&card, &ops, &f); f.malformed = 0; n = sizeof atr;
    assert(!mb86H_card_power_on(&card, atr, &n));
    f.generation_mismatch = 1; n = sizeof rx;
    assert(mb86H_card_transfer(&card, tx, 5, rx, &n) == -EPROTO);
    puts("card: chunking, limits, generation, runtime rejection and no replay passed");
    return 0;
}
