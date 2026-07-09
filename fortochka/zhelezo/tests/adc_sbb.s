# EXPECT: exit=hlt ebx=0x1 edx=0xffffffff cf=1
	.intel_syntax noprefix
	.text
	mov eax, 0xFFFFFFFF
	add eax, 1              # CF=1
	mov ebx, 0
	adc ebx, 0              # ebx=1, CF=0
	stc
	sbb edx, edx            # edx = 0-0-1 = -1, CF stays 1
	hlt
