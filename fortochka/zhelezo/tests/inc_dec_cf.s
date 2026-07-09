# INC/DEC preserve CF — the classic interpreter bug.
# EXPECT: exit=hlt ebx=0x1 eax=0xffffffff cf=1
	.intel_syntax noprefix
	.text
	stc
	mov eax, 0xFFFFFFFF
	inc eax                 # wraps to 0, ZF=1, CF must stay 1
	jnc fail
	jnz fail
	dec eax                 # back to -1, CF still 1
	jnc fail
	mov ebx, 1
	hlt
fail:
	mov ebx, 0xdead
	hlt
