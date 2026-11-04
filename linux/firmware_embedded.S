/* Embeds the GV-MVP/XZ resident bundle into the module.  FIRMWARE_BUNDLE is set by
 * linux/Makefile to mb86H/firmware/firmware.bin written by fw/build.py. */
	.section .rodata
	.balign 16
	.global firmware_embedded_data
firmware_embedded_data:
	.incbin FIRMWARE_BUNDLE
	.global firmware_embedded_end
firmware_embedded_end:
	.previous
	.section .note.GNU-stack,"",@progbits
