	.intel_syntax noprefix
	.text
	mov edi, 0x8000
	xor esi, esi
	mov ecx, 30000000
lp:	mov eax, [edi+esi*4+0x100]
	mov [edi+esi*4+0x200], eax
	add eax, dword ptr [edi+0x300]
	mov edx, fs:[0x18]
	movzx ebx, word ptr [edi+esi*2+0x400]
	dec ecx
	jnz lp
	hlt
