	.intel_syntax noprefix
	.text
	mov ecx, 50000000
	xor eax, eax
lp:	add eax, 3
	xor eax, 5
	dec ecx
	jnz lp
	hlt
