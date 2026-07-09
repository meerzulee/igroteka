# x87 integer-exact arithmetic: fild/fmulp/fidiv/fsqrt/faddp/fistp round trips.
# EXPECT: exit=hlt eax=42 ebx=25 ecx=12 edx=13
	.intel_syntax noprefix
	.text
	mov dword ptr [0x8000], 6
	mov dword ptr [0x8004], 7
	# 6 * 7 = 42
	fild dword ptr [0x8000]
	fild dword ptr [0x8004]
	fmulp st(1), st(0)
	fistp dword ptr [0x8020]
	mov eax, [0x8020]
	# 100 / 4 = 25   (fidiv: st0 = st0 / mem)
	mov dword ptr [0x8008], 100
	mov dword ptr [0x800c], 4
	fild dword ptr [0x8008]
	fidiv dword ptr [0x800c]
	fistp dword ptr [0x8024]
	mov ebx, [0x8024]
	# sqrt(144) = 12
	mov dword ptr [0x8010], 144
	fild dword ptr [0x8010]
	fsqrt
	fistp dword ptr [0x8028]
	mov ecx, [0x8028]
	# 6 + 7 = 13
	fild dword ptr [0x8000]
	fild dword ptr [0x8004]
	faddp st(1), st(0)
	fistp dword ptr [0x802c]
	mov edx, [0x802c]
	hlt
