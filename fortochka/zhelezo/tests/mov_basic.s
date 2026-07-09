# EXPECT: exit=hlt eax=0x12349978 ebx=0x12345678 edx=0x12345678 esi=0xab edi=0xcdef
	.intel_syntax noprefix
	.text
	mov eax, 0x12345678
	mov ebx, eax
	mov ecx, 0x8000
	mov [ecx], eax
	mov edx, [ecx]
	mov byte ptr [ecx+4], 0xAB
	movzx esi, byte ptr [ecx+4]
	mov word ptr [ecx+6], 0xCDEF
	movzx edi, word ptr [ecx+6]
	mov ah, 0x99
	hlt
