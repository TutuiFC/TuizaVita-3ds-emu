.arch armv7a

.section .vitalink.fstubs.aznative,"ax",%progbits

	.align 4
	.global AzNativeWorldUnshare
	.type AzNativeWorldUnshare, %function
AzNativeWorldUnshare:
.if GEN_WEAK_EXPORTS
	.word 0x00010008
.else
	.word 0x00010000
.endif //GEN_WEAK_EXPORTS
	.word 0xA36C8960
	.word 0x5240AF61
	.align 4

