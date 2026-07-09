# EXPECT: exit=hlt eax=0xfffffff9 edx=0xffffffff ebx=0xb2d05e00 esi=0x1
	.intel_syntax noprefix
	.text
	mov eax, 1000000
	mov ecx, 3000
	mul ecx                 # 3e9 fits 32 bits: edx=0, CF=0
	jc fail
	mov ebx, eax            # 0xB2D05E00
	mov eax, 0x10000
	mul eax                 # 2^32: eax=0 edx=1 CF=1
	jnc fail
	mov esi, edx
	mov eax, -50
	cdq
	mov ecx, 7
	idiv ecx                # -50/7 = -7 rem -1
	hlt
fail:
	mov eax, 0xdead
	hlt
