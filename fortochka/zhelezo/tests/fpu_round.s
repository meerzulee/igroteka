# fist/fistp must honor the control-word rounding mode. C's (int)float sets
# RC=truncate (bits 10-11 = 11 → control 0x0F7F) before fistp. 27/10 = 2.7
# truncates to 2, -27/10 = -2.7 truncates to -2 (toward zero, not -3).
# Then round-to-nearest (control 0x037F): 2.7 → 3.
# EXPECT: exit=hlt eax=0x2 ebx=0xfffffffe ecx=0x3
	.intel_syntax noprefix
	.text
	mov word ptr [0x8000], 0x0F7F   # RC = truncate
	fldcw word ptr [0x8000]
	mov dword ptr [0x8008], 10
	mov dword ptr [0x8004], 27
	fild dword ptr [0x8004]
	fidiv dword ptr [0x8008]         # 2.7
	fistp dword ptr [0x800c]
	mov eax, [0x800c]                # 2 (truncated)
	mov dword ptr [0x8004], -27
	fild dword ptr [0x8004]
	fidiv dword ptr [0x8008]         # -2.7
	fistp dword ptr [0x8010]
	mov ebx, [0x8010]                # -2 (toward zero)
	mov word ptr [0x8000], 0x037F    # RC = nearest
	fldcw word ptr [0x8000]
	mov dword ptr [0x8004], 27
	fild dword ptr [0x8004]
	fidiv dword ptr [0x8008]         # 2.7
	fistp dword ptr [0x8014]
	mov ecx, [0x8014]                # 3 (nearest)
	hlt
