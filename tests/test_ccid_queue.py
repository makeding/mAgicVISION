#!/usr/bin/env python3
"""Exercise the actual CCID queue functions with inline gadget completions."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--cc", default="cc")
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / "linux/ccid.c").read_text()

def function(name):
    import re
    match = re.search(r"static (?:inline )?[\w *]+\b" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise RuntimeError("missing production function " + name)
    start = match.start()
    pos = source.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

work_source = function("ccid_work")
assert work_source.index("ccid_extension_begin") < work_source.index("mutex_lock(&c->backend_lock)")
assert work_source.index("mutex_lock(&c->backend_lock)") < work_source.index("ccid_status_locked")
assert "command==PC_TO_RDR_POWER_ON || command==PC_TO_RDR_XFR_BLOCK" in work_source
assert work_source.count("ccid_extension_begin") == 1
out_source = function("ccid_out_complete")
assert "ccid_queue_final" in out_source[out_source.index("bad:"):]
assert "ccid_queue_in" not in source

prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define CCID_IN_BYTES 522u
typedef int gfp_t;
typedef struct { bool held; } spinlock_t;
struct work_struct { bool pending; };
struct delayed_work { struct work_struct work; };
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
struct mb86H_ccid_ops { int (*status)(void *, u8 *); };
struct usb_function { int unused; };
struct usb_ep { unsigned id; bool enabled; };
struct usb_request { void *buf, *context; unsigned length, zero; int status; };
struct ccid_function {
    struct usb_function function;
    struct usb_ep *in_ep, *out_ep, *intr_ep;
    struct usb_request *in_req, *out_req, *intr_req;
    struct work_struct work, io_work;
    struct delayed_work slot_work;
	struct delayed_work extension_work;
    struct mutex backend_lock;
    const struct mb86H_ccid_ops *ops;
    void *opaque;
    int presence_error;
    spinlock_t state_lock, submit_lock;
    bool registered, online, out_queued, in_flight, intr_queued, slot_change_pending;
    int abort_token;
    u8 slot_change_value;
    u32 generation, backend_generation, command_backend_generation;
    size_t frame_len, expected_len;
    bool work_pending, in_pending, parameters_valid;
	bool extension_active, final_pending;
	u8 extension_type, extension_slot, extension_seq, extension_multiplier,extension_status;
	u8 in_owner, final_attempts;
	u32 command_epoch, command_work_epoch;
	u32 extension_generation, extension_backend_generation, extension_epoch;
	u32 final_generation, final_epoch;
	size_t final_length;
	u8 final[CCID_IN_BYTES];
	u8 cached_icc;
	bool cached_icc_valid;
};
#define CCID_HEADER 10u
#define CCID_OUT_PACKET 512u
#define GFP_ATOMIC 0
#define ESHUTDOWN 108
#define EMSGSIZE 90
#define EIO 5
#define EPROTO 71
#define MB86H_CARD_ABSENT 2
#define CCID_PRESENCE_INTERVAL_MS 250u
#define CCID_EXTENSION_INTERVAL_MS 1000u
#define CCID_FINAL_RETRY_MS 20u
#define CCID_FINAL_MAX_RETRIES 3u
#define CCID_FINAL_MAX_ATTEMPTS (1u + CCID_FINAL_MAX_RETRIES)
#define RDR_TO_PC_DATABLOCK 0x80
#define RDR_TO_PC_SLOTSTATUS 0x81
#define pr_warn(...) ((void)0)
#define pr_err_ratelimited(...) ((void)0)
#define READ_ONCE(x) (x)
#define container_of(ptr, type, member) ((type *)((char *)(ptr)-offsetof(type, member)))
static void spin_lock(spinlock_t *lock) { assert(!lock->held); lock->held=true; }
static void spin_unlock(spinlock_t *lock) { assert(lock->held); lock->held=false; }
#define spin_lock_irqsave(lock, flags) do { (flags)=0; spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); spin_unlock(lock); } while (0)
static void atomic_set(int *p, int value) { *p=value; }
static void ccid_put_le32(u8 *p,u32 n) {p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static bool schedule_work(struct work_struct *work) {
    bool queued=!work->pending; work->pending=true; return queued;
}
static unsigned msecs_to_jiffies(unsigned ms) { return ms; }
static bool schedule_delayed_work(struct delayed_work *d, unsigned delay) {
    (void)delay; return schedule_work(&d->work);
}
static void *system_wq;
static bool mod_delayed_work(void *wq,struct delayed_work *d,unsigned delay) {
    (void)wq;(void)delay;d->work.pending=true;return true;
}
static bool cancel_delayed_work(struct delayed_work *d) {
    bool pending=d->work.pending; d->work.pending=false; return pending;
}
static bool cancel_delayed_work_sync(struct delayed_work *d) {
    return cancel_delayed_work(d);
}
static struct ccid_function *func_to_ccid(struct usb_function *f) {
    return container_of(f, struct ccid_function, function);
}
static void ccid_in_complete(struct usb_ep *, struct usb_request *);
static void ccid_intr_complete(struct usb_ep *, struct usb_request *);
static int usb_ep_queue(struct usb_ep *, struct usb_request *, gfp_t);
static int usb_ep_disable(struct usb_ep *);
"""
names = ["ccid_submit_out", "ccid_notify_slot_change", "ccid_reply",
         "ccid_final_current", "ccid_stage_final_locked", "ccid_queue_final",
         "ccid_extension_work", "ccid_extension_begin",
         "ccid_in_complete", "ccid_intr_complete", "ccid_disable", "ccid_slot_work", "ccid_backend_changed"]
if "ccid_io_submit" in source:
    production = "enum ccid_endpoint { CCID_INTR, CCID_IN, CCID_OUT };\n"
    production += "enum ccid_in_owner { CCID_IN_NONE, CCID_IN_EXTENSION, CCID_IN_FINAL };\n"
    production += function("ccid_io_submit") + "\n" + function("ccid_io_work") + "\n"
else:
    production = ""
production += "\n".join(function(name) for name in names)
tests = r"""
static unsigned queued[3], depth, max_depth;
static bool sync_in, sync_intr, notify_during_intr;
static int fail_id=-1;
static int usb_ep_queue(struct usb_ep *ep, struct usb_request *req, gfp_t flags) {
    struct ccid_function *c=req->context;
    (void)flags;
    /* Completion immediately retakes this lock, as in noa's failing stack. */
    assert(!c->state_lock.held);
    assert(ep->enabled && c->online);
    ++depth; if(depth>max_depth)max_depth=depth;
    assert(depth==1);
    ++queued[ep->id];
    if((int)ep->id==fail_id) { --depth; return -EIO; }
    req->status=0;
    if(ep==c->intr_ep && sync_intr) {
        if(notify_during_intr) {
            notify_during_intr=false;
            ccid_notify_slot_change(c, GFP_ATOMIC);
        }
        ccid_intr_complete(ep,req);
    } else if(ep==c->in_ep && sync_in) {
        ccid_in_complete(ep,req);
    }
    --depth;
    return 0;
}
static int usb_ep_disable(struct usb_ep *ep) {
    extern struct ccid_function fixture;
    struct ccid_function *c=&fixture;
    assert(!c->state_lock.held);
    ep->enabled=false;
    if(ep==c->intr_ep && c->intr_queued) {
        c->intr_req->status=-ESHUTDOWN;
        ccid_intr_complete(ep,c->intr_req);
    } else if(ep==c->in_ep && c->in_flight) {
        c->in_req->status=-ESHUTDOWN;
        ccid_in_complete(ep,c->in_req);
    } else if(ep==c->out_ep) {
        spin_lock(&c->state_lock); c->out_queued=false; spin_unlock(&c->state_lock);
    }
    return 0;
}
struct ccid_function fixture;
static struct usb_ep eps[3];
static struct usb_request reqs[3];
static unsigned char buffers[3][CCID_IN_BYTES];
static void init(void) {
    memset(&fixture,0,sizeof fixture);memset(queued,0,sizeof queued);
    memset(reqs,0,sizeof reqs);depth=max_depth=0;
    sync_in=sync_intr=true;notify_during_intr=false;fail_id=-1;
    for(unsigned i=0;i<3;++i) {
        eps[i]=(struct usb_ep){.id=i,.enabled=true};
        reqs[i].context=&fixture;reqs[i].buf=buffers[i];
    }
    fixture.in_ep=&eps[0];fixture.out_ep=&eps[1];fixture.intr_ep=&eps[2];
    fixture.in_req=&reqs[0];fixture.out_req=&reqs[1];fixture.intr_req=&reqs[2];
    fixture.online=true;fixture.generation=7;fixture.slot_change_value=2;
}
static void pump(void) {
    unsigned calls=0;
    while(fixture.io_work.pending) {
        assert(++calls<20);
        /* Linux clears PENDING before executing a work item. */
        fixture.io_work.pending=false;
        ccid_io_work(&fixture.io_work);
    }
}
static u8 observed_status;
static int observed_error;
static unsigned status_queries;
static int card_status(void *opaque, u8 *status) {
    assert(opaque==&fixture);
    ++status_queries;*status=observed_status;return observed_error;
}
static const struct mb86H_ccid_ops status_ops={card_status};
static void poll_slot(void) {
    fixture.slot_work.work.pending=false;
    ccid_slot_work(&fixture.slot_work.work);
}
int main(void) {
    u8 reply[12]={0x80};
    init();
    sync_in=false;fixture.work_pending=true;fixture.backend_generation=4;
    fixture.command_work_epoch=3;
	ccid_extension_begin(&fixture,RDR_TO_PC_DATABLOCK,0,9,7,4,3);
	fixture.extension_work.work.pending=false;
	ccid_extension_work(&fixture.extension_work.work);
    assert(fixture.extension_work.work.pending); /* Periodic even while IN is busy. */
	pump();
	assert(fixture.in_flight && fixture.in_owner==CCID_IN_EXTENSION && queued[0]==1);
    assert(buffers[0][0]==RDR_TO_PC_DATABLOCK && buffers[0][5]==0 && buffers[0][6]==9);
    assert(buffers[0][7]==0x82 && buffers[0][8]==1 && buffers[0][9]==0);
    fixture.extension_work.work.pending=false;
    ccid_extension_work(&fixture.extension_work.work);
    assert(fixture.extension_work.work.pending && queued[0]==1);
	assert(!ccid_queue_final(&fixture,reply,sizeof(reply),7,3));
    assert(fixture.final_pending && fixture.work_pending && !fixture.out_queued);
	ccid_in_complete(fixture.in_ep,fixture.in_req);pump();
    assert(fixture.in_flight && fixture.in_owner==CCID_IN_FINAL && queued[0]==2);
    assert(fixture.final_pending && fixture.work_pending && !memcmp(buffers[0],reply,sizeof(reply)));
    ccid_in_complete(fixture.in_ep,fixture.in_req);pump();
    assert(!fixture.final_pending && !fixture.work_pending && fixture.out_queued);
    notify_during_intr=true;
    ccid_notify_slot_change(&fixture,GFP_ATOMIC);
    pump();
    assert(queued[2]==2 && !fixture.intr_queued && !fixture.slot_change_pending);
    assert(fixture.out_queued && max_depth==1);

    /* A final IN queue failure retains the exact response and command owner;
     * delayed retry sends it without another backend call or an OUT window. */
    init();sync_in=false;fixture.work_pending=true;fixture.command_work_epoch=11;
    fail_id=0;
    assert(!ccid_queue_final(&fixture,reply,sizeof(reply),7,11));pump();
    assert(fixture.final_pending && fixture.work_pending && !fixture.out_queued);
    assert(fixture.final_attempts==1 && fixture.extension_work.work.pending);
    fail_id=-1;fixture.extension_work.work.pending=false;
    ccid_extension_work(&fixture.extension_work.work);pump();
    assert(fixture.in_flight && fixture.in_owner==CCID_IN_FINAL && fixture.final_attempts==2);
    ccid_in_complete(fixture.in_ep,fixture.in_req);pump();
    assert(!fixture.final_pending && !fixture.work_pending && fixture.out_queued);

    /* Exhaustion fences the command; it never drops final or opens OUT. */
    init();sync_in=false;fixture.work_pending=true;fixture.command_work_epoch=15;
    fail_id=0;
    assert(!ccid_queue_final(&fixture,reply,sizeof(reply),7,15));pump();
    for(unsigned attempt=1;attempt<CCID_FINAL_MAX_ATTEMPTS;++attempt) {
        assert(fixture.extension_work.work.pending);
        fixture.extension_work.work.pending=false;
        ccid_extension_work(&fixture.extension_work.work);pump();
    }
    assert(fixture.final_attempts==CCID_FINAL_MAX_ATTEMPTS);
    assert(fixture.final_pending && fixture.work_pending && !fixture.out_queued);
    assert(!fixture.extension_work.work.pending);

    /* Inline final completion is also terminal exactly once. */
    init();fixture.work_pending=true;fixture.command_work_epoch=12;
    assert(!ccid_queue_final(&fixture,reply,sizeof(reply),7,12));pump();
    assert(queued[0]==1 && !fixture.final_pending && !fixture.work_pending);
    assert(fixture.out_queued && max_depth==1);

    /* A stopped backend retires extensions but cannot eat a terminal reply
     * that was already built under backend_lock. */
    init();sync_in=false;fixture.work_pending=true;fixture.command_work_epoch=13;
    fixture.backend_generation=2;
    ccid_extension_begin(&fixture,RDR_TO_PC_DATABLOCK,0,4,7,2,13);
    fixture.in_flight=true;fixture.in_owner=CCID_IN_EXTENSION;
    assert(!ccid_queue_final(&fixture,reply,sizeof(reply),7,13));
    ccid_backend_changed(&fixture);
    assert(!fixture.extension_active && fixture.final_pending);
    fixture.in_flight=false;fixture.in_owner=CCID_IN_NONE;
    fixture.extension_work.work.pending=false;
    ccid_extension_work(&fixture.extension_work.work);pump();
    assert(fixture.in_flight && fixture.in_owner==CCID_IN_FINAL);
    ccid_in_complete(fixture.in_ep,fixture.in_req);pump();
    assert(fixture.out_queued);

    /* An old worker releases its old owner after disable/re-enable; it never
     * publishes its stale response into the new USB generation. */
    init();fixture.generation=8;fixture.work_pending=true;fixture.command_work_epoch=14;
    assert(ccid_queue_final(&fixture,reply,sizeof(reply),7,14)==-ESHUTDOWN);
    pump();assert(!fixture.work_pending && fixture.out_queued && !queued[0]);

    init();
    sync_intr=false;
    ccid_notify_slot_change(&fixture,GFP_ATOMIC);
    pump();assert(fixture.intr_queued);
    ccid_disable(&fixture.function);
    pump();assert(!fixture.intr_queued && !fixture.out_queued);

    init();
    fail_id=2;
    ccid_notify_slot_change(&fixture,GFP_ATOMIC);
    pump();
    assert(!fixture.intr_queued && fixture.slot_change_pending && queued[2]==1);
    fail_id=-1;ccid_submit_out(&fixture,GFP_ATOMIC);pump();
    assert(!fixture.slot_change_pending && queued[2]==2);

    init();
    fixture.registered=true;fixture.ops=&status_ops;fixture.opaque=&fixture;
    observed_status=MB86H_CARD_ABSENT;observed_error=0;status_queries=0;
    poll_slot();pump();
    assert(fixture.slot_work.work.pending && queued[2]==0);
    observed_status=1;
    poll_slot();pump();
    assert(fixture.slot_change_value==3 && queued[2]==1);
    poll_slot();pump();assert(queued[2]==1);
    observed_error=-EIO;observed_status=MB86H_CARD_ABSENT;
    poll_slot();pump();
    assert(fixture.slot_change_value==3 && queued[2]==1);
    observed_error=0;
    poll_slot();pump();
    assert(fixture.slot_change_value==2 && queued[2]==2);
    assert(status_queries==5);
    ccid_disable(&fixture.function);
    assert(!fixture.slot_work.work.pending);
    poll_slot();assert(status_queries==5 && !fixture.slot_work.work.pending);
    init();
    fixture.command_backend_generation=fixture.backend_generation;
    fixture.parameters_valid=true;fixture.frame_len=8;fixture.expected_len=20;
    ccid_backend_changed(&fixture);
    assert(fixture.command_backend_generation!=fixture.backend_generation);
    assert(!fixture.parameters_valid && !fixture.frame_len && !fixture.expected_len);
    ccid_backend_changed(&fixture);
    assert(fixture.backend_generation==2);
    puts("CCID inline completion, command handoff, backend epoch, card insertion/removal and disable: passed");
}
"""
args.out.parent.mkdir(parents=True, exist_ok=True)
unit = args.out.with_suffix(".c")
unit.write_text(prelude + production + tests)
subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(unit), "-o", str(args.out)], check=True)
subprocess.run([str(args.out)], check=True)
