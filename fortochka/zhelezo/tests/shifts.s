# EXPECT: exit=hlt eax=0x1 ebx=0xf8000000 ecx=0x1 edx=0x12345678
	.intel_syntax noprefix
	.text
	mov eax, 0x80000001
	shl eax, 1              # CF=1
	jnc fail
	mov ebx, 0x80000000
	sar ebx, 4              # sign-fill: 0xF8000000
	mov ecx, 3
	shr ecx, 1              # ecx=1, CF=1
	jnc fail
	mov edx, 0x12345678
	rol edx, 8
	ror edx, 8              # round trip
	mov esi, 1
	rcr esi, 1              # bit 0 -> CF
	jnc fail
	mov eax, 1
	hlt
fail:
	mov eax, 0xdead
	hlt
