#include "mb86H/usb/window.h"
#include "mb86H/protocol/constants.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct mock { unsigned count, fail; struct usb_window_control controls[64]; uint8_t data[64][64]; };
static int control(void *opaque,const struct usb_window_control *c,void *bytes,unsigned timeout)
{
    struct mock *m=opaque; unsigned i=m->count++;
    assert(i<64 && c->length<=64 && (timeout==1000 || timeout==20000));
    m->controls[i]=*c;
    if(c->request_type==0x40) memcpy(m->data[i],bytes,c->length);
    else memset(bytes,0xa5,c->length);
    return m->fail && m->count==m->fail ? -777 : 0;
}
int main(void)
{
    struct usb_native_ops ops={.control=control};
    struct mock m={0}; uint8_t tx[64],rx[64];
    for(unsigned i=0;i<64;++i) tx[i]=(uint8_t)i;
    assert(!usb_xz_exchange(&ops,&m,tx,rx)); assert(m.count==17);
    for(unsigned i=0;i<16;++i) {
        uint32_t address=MB86H_BOOT0_STAGING_ADDRESS+60-4*i;
        assert(m.controls[i].request_type==0x40 && m.controls[i].request==0xbc);
        assert(m.controls[i].value==((address>>16)&15)*256);
        assert(m.controls[i].index==(uint16_t)address && m.controls[i].length==4);
        assert(!memcmp(m.data[i],tx+60-4*i,4));
    }
    assert(m.controls[16].request_type==0xc0 && m.controls[16].length==64);
    for(unsigned fail=1;fail<=17;++fail) {
        m=(struct mock){.fail=fail};
        assert(usb_xz_exchange(&ops,&m,tx,rx)==-777 && m.count==fail);
    }
    m=(struct mock){0};
    assert(!usb_xz_window(&ops,&m,0xe9cf6,rx,64,false) && m.count==1);
    m=(struct mock){0};
    assert(!usb_xz_window(&ops,&m,0xe9cf8,tx,64,true) && m.count==16);
    assert(usb_xz_window(&ops,&m,0xfffff,rx,2,false)==-EINVAL);
    assert(usb_xz_window(&ops,&m,0xe9cf9,tx,4,true)==-EINVAL);
    puts("PASS XZ EP0 exact setup/data order, address bounds, no replay on all 17 failures");
    return 0;
}
