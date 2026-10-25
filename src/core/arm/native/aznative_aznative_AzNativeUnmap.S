.arch armv7a

.section .vitalink.fstubs.aznative,"ax",%progbits

	.align 4
	.global AzNativeUnmap
	.type AzNativeUnmap, %function
AzNativeUnmap:
.if GEN_WEAK_EXPORTS
	.word 0x00010008
.else
	.word 0x00010000
.endif //GEN_WEAK_EXPORTS
	.word 0xA36C8960
	.word 0x04A6A5E0
	.align 4

