# Out-of-arena load: precise fault, EIP at the faulting mov, address reported.
# EXPECT: exit=fault fault=memread fault_addr=0x1000004 eip=0x1005
	.intel_syntax noprefix
	.text
	mov eax, 0x00FFFFFC
	mov ebx, [eax+8]        # 0x1000004 is past the 16 MB arena
	hlt
