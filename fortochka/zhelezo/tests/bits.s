# EXPECT: exit=hlt ebp=0x1 ecx=0x63 edx=0x10c esi=0x7 eax=0x7 ebx=0x3
	.intel_syntax noprefix
	.text
	mov eax, 0x1000
	bsf ecx, eax            # 12
	jz fail
	bsr edx, eax            # 12
	bt eax, 12
	jnc fail
	setc dh                 # edx = 0x010C
	mov esi, 5
	mov edi, 7
	cmp esi, edi
	cmovl esi, edi          # taken: esi=7
	mov eax, 10
	mov ecx, 10
	mov ebx, 99
	cmpxchg ecx, ebx        # equal: ZF=1, ecx=99
	jne fail
	mov eax, 3
	mov ebx, 4
	xadd eax, ebx           # eax=7 ebx=3
	cmp eax, 7
	jne fail
	mov ebp, 1
	hlt
fail:
	mov ebp, 0xdead
	hlt
