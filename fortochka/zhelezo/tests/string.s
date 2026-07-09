# EXPECT: exit=hlt eax=0x1 ebx=0x55667788 edx=0xdeadbeef ecx=0x0
	.intel_syntax noprefix
	.text
	cld
	mov esi, 0x8000
	mov edi, 0x9000
	mov eax, 0x11223344
	mov [esi], eax
	mov dword ptr [esi+4], 0x55667788
	mov ecx, 8
	rep movsb
	mov ebx, [0x9004]
	mov edi, 0xA000
	mov eax, 0xDEADBEEF
	mov ecx, 4
	rep stosd
	mov edx, [0xA00C]
	mov esi, 0x8000
	mov edi, 0x9000
	mov ecx, 8
	repe cmpsb              # identical buffers: runs dry, ZF=1
	jne fail
	mov eax, 1
	hlt
fail:
	mov eax, 0xdead
	hlt
