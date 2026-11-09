@ The launch splash (cia/splash.png, 400x240, drawn from cia/splash.svg): on the top screen while
@ the library is read, so nobody watches a count.
	.section .rodata
	.global splash_png
	.global splash_png_end
	.balign 4
splash_png:
	.incbin "cia/splash.png"
splash_png_end:
	.byte 0
