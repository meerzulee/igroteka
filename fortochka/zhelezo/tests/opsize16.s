# 0x66-prefixed 16-bit operations must not touch upper halves.
# EXPECT: exit=hlt eax=0xaabb0122 ecx=0x122 edx=0x1
	.intel_syntax noprefix
	.text
	mov eax, 0xAABBCCDD
	mov bx, 0x1122
	mov ax, bx              # eax=0xAABB1122
	add ax, 0xF000          # 16-bit carry out: ax=0x0122, CF=1
	jnc fail
	movzx ecx, ax
	mov edx, 1
	hlt
fail:
	mov edx, 0xdead
	hlt
