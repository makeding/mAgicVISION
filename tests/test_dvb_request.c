#include "mb86H/frontend.h"
#include "mb86H/device.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

int main(void)
{
    const struct mb86H_device_info *torne =
        mb86H_device_find(USB_TORNE_VENDOR_ID, USB_TORNE_PRODUCT_ID);
    CHECK(torne == mb86H_torne_device());
    CHECK(!strcmp(torne->name, "Sony torne CECH-ZD1 J ISDB-T Tuner"));
    CHECK(torne->frontend_count == 1 && torne->delivery_systems == (1U << MB86H_DVB_GR));
    CHECK(mb86H_device_find(USB_VENDOR_ID, USB_PRODUCT_ID) == mb86H_gv_mvp_xz_device());
    CHECK(mb86H_gv_mvp_xz_device()->frontend_count == 2);
    CHECK(!mb86H_device_find(USB_TORNE_VENDOR_ID, USB_PRODUCT_ID));
    CHECK(!mb86H_device_find(USB_VENDOR_ID, USB_TORNE_PRODUCT_ID));
    CHECK(!mb86H_device_find(0, 0));
    CHECK(!mb86H_gv_mvp_xz_matches(USB_TORNE_VENDOR_ID, USB_TORNE_PRODUCT_ID));
    CHECK(!strcmp(mb86H_device_name(USB_TORNE_VENDOR_ID, USB_TORNE_PRODUCT_ID), torne->name));
	CHECK(!strcmp(mb86H_gv_mvp_xz_device_name(USB_VENDOR_ID, USB_PRODUCT_ID), "I-O DATA GV-MVP/XZ ISDB-T/S Tuner"));
	CHECK(!strcmp(mb86H_gv_mvp_xz_device_name(0, USB_PRODUCT_ID), "mb86H device"));
	CHECK(!strcmp(mb86H_gv_mvp_xz_device_name(USB_VENDOR_ID, 0), "mb86H device"));
	struct mb86H_dvb_caps caps[2] = {
		{473142857, 707143000, 1, 192, 0, 0},
		{950000, 2150000, 1, 188, 1, 1},
	};
	struct mb86H_dvb_request r = {
		.system = MB86H_DVB_GR, .frequency = 473142857,
		.bandwidth_hz = 6000000, .stream_id = (uint32_t)-1, .layers = 7,
	};
	CHECK(mb86H_dvb_caps_valid(caps));
	CHECK(mb86H_dvb_request_valid(caps, &r));
	r.frequency = 551142857; /* recisdb T26, unchanged Hz */
	CHECK(mb86H_dvb_request_valid(caps, &r));
	r.frequency = 551142;
	CHECK(!mb86H_dvb_request_valid(caps, &r));
	r.frequency = 551142857;
	r.bandwidth_hz = 8000000;
	CHECK(!mb86H_dvb_request_valid(caps, &r));
	r.bandwidth_hz = 0;
	CHECK(mb86H_dvb_request_valid(caps, &r));
	r.sound_broadcasting = 1;
	CHECK(!mb86H_dvb_request_valid(caps, &r));
	r.sound_broadcasting = 0;
	r.partial_reception = 1;
	CHECK(!mb86H_dvb_request_valid(caps, &r));
	r.partial_reception = 0;
	r.layers = 8;
	CHECK(!mb86H_dvb_request_valid(caps, &r));
	r.layers = 7;
	r.stream_id = 1;
	CHECK(!mb86H_dvb_request_valid(caps, &r));
	r.system = MB86H_DVB_BSCS;
	r.frequency = 1202920; /* BS09 IF kHz */
	r.stream_id = 16528; /* absolute TSID, not slot zero */
	CHECK(mb86H_dvb_request_valid(caps + 1, &r));
	r.frequency = 1202920000;
	CHECK(!mb86H_dvb_request_valid(caps + 1, &r));
	r.frequency = 1202920;
	r.stream_id = 0x10000;
	CHECK(!mb86H_dvb_request_valid(caps + 1, &r));
	r.stream_id = (uint32_t)-1;
	CHECK(mb86H_dvb_request_valid(caps + 1, &r));
	caps[1].auto_tsid = 0;
	CHECK(!mb86H_dvb_request_valid(caps + 1, &r));
	r.stream_id = 0xffff;
	CHECK(mb86H_dvb_request_valid(caps + 1, &r));
	r.system = 2;
	CHECK(!mb86H_dvb_request_valid(caps + 1, &r));
	caps[1].mux_channel = 0;
	CHECK(!mb86H_dvb_caps_valid(caps));
	caps[1].mux_channel = 1;
	caps[1].record_size = 204;
	CHECK(!mb86H_dvb_caps_valid(caps));
	caps[1].present = 0;
	CHECK(mb86H_dvb_caps_valid(caps));
	caps[0].frequency_min = 0;
	CHECK(!mb86H_dvb_caps_valid(caps));
	caps[0].present = 0;
	CHECK(!mb86H_dvb_caps_valid(caps));
	CHECK(!mb86H_dvb_caps_valid(NULL));
	CHECK(!mb86H_dvb_request_valid(NULL, &r));
	CHECK(!mb86H_dvb_request_valid(caps, NULL));
	puts("DVB request validation passed");
	return 0;
}
