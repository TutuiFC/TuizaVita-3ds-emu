.arch armv7a

.section .vitalink.fstubs.aznative,"ax",%progbits

	.align 4
	.global AzNativeWorldWrite
	.type AzNativeWorldWrite, %function
AzNativeWorldWrite:
.if GEN_WEAK_EXPORTS
	.word 0x00010008
.else
	.word 0x00010000
.endif //GEN_WEAK_EXPORTS
	.word 0xA36C8960
	.word 0x39346431
	.align 4

