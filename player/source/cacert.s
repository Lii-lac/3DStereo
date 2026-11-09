@ The CA bundle (nettest/cacert.pem, Mozilla's, via curl.se) baked into the app; spotify.c
@ writes it to the card for curl.
	.section .rodata
	.global cacert_pem
	.global cacert_pem_end
	.balign 4
cacert_pem:
	.incbin "source/cacert.pem"
cacert_pem_end:
	.byte 0
