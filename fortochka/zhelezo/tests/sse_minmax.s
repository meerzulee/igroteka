# SSE min/max NaN + signed-zero (return src), and cvttss2si out-of-range → 0x80000000.
# maxss(NaN, 5.0) → 5.0 (src); maxss(-0.0,+0.0) → +0.0 so movmskps=0; cvttss2si(+inf)=0x80000000
# EXPECT: exit=hlt eax=5 ebx=0 ecx=0x80000000
	.intel_syntax noprefix
	.text
	# maxss(NaN, 5.0) must yield src 5.0
	mov dword ptr [0x8000], 0x7FC00000   # qNaN
	movss xmm0, [0x8000]
	mov eax, 5
	cvtsi2ss xmm1, eax                   # 5.0
	maxss xmm0, xmm1                     # NaN unordered → src = 5.0
	cvttss2si eax, xmm0                 # 5
	# maxss(-0.0, +0.0) must yield src +0.0 (positive) → movmskps lane0 = 0
	mov dword ptr [0x8004], 0x80000000   # -0.0
	movss xmm2, [0x8004]
	xorps xmm3, xmm3                     # +0.0 (all lanes)
	maxss xmm2, xmm3                     # equal-zero → src = +0.0
	movmskps ebx, xmm2                   # 0 (all lanes non-negative)
	# cvttss2si(+inf) → integer indefinite 0x80000000
	mov dword ptr [0x8008], 0x7F800000   # +inf
	movss xmm4, [0x8008]
	cvttss2si ecx, xmm4                 # 0x80000000
	hlt
