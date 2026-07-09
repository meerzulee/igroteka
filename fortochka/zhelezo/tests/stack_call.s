# EXPECT: exit=hlt eax=0x1234 ebx=0xaa55 ecx=0x42 edx=0x1
	.intel_syntax noprefix
	.text
	mov edi, esp
	push 0x1234
	pop eax
	call fn
	cmp esp, edi            # call/ret balanced
	jne fail
	push ebp                # standard prologue/epilogue round trip
	mov ebp, esp
	sub esp, 0x10
	mov dword ptr [ebp-4], 0xAA55
	mov ebx, [ebp-4]
	leave
	cmp esp, edi
	jne fail
	mov edx, 1
	hlt
fail:
	mov edx, 0xdead
	hlt
fn:
	mov ecx, 0x42
	ret
