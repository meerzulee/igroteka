# Self-checking: every flag assertion is a branch; landing in fail poisons eax.
# EXPECT: exit=hlt eax=0x1
	.intel_syntax noprefix
	.text
	mov eax, 0xFFFFFFFF
	add eax, 1              # 0, CF=1 ZF=1
	jnz fail
	jnc fail
	mov ebx, 0x7FFFFFFF
	add ebx, 1              # signed overflow: OF=1 SF=1
	jno fail
	jns fail
	mov ecx, 5
	sub ecx, 7              # borrow: CF=1 SF=1
	jnc fail
	jns fail
	cmp ecx, -2
	jne fail
	xor edx, edx            # logic clears CF/OF, sets ZF
	jnz fail
	jc fail
	mov eax, 1
	hlt
fail:
	mov eax, 0xdead
	hlt
