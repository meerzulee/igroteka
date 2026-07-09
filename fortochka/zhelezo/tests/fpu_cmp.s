# x87 compare → condition codes → fnstsw ax. 5 < 9 sets C0 (bit 8 = 0x100).
# EXPECT: exit=hlt eax=0x100
	.intel_syntax noprefix
	.text
	mov dword ptr [0x8000], 5
	mov dword ptr [0x8004], 9
	fild dword ptr [0x8000]   # st0 = 5
	fild dword ptr [0x8004]   # st0 = 9, st1 = 5
	fxch st(1)                # st0 = 5, st1 = 9
	fcomp st(1)               # compare st0(5) vs st1(9): 5 < 9 → C0 set; pop
	fnstsw ax
	and eax, 0x4500           # isolate C3|C2|C0
	hlt
