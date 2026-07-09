# SSE1 packed single + movaps + movmskps + comiss + xorps-zero.
# addps of {1,2,3,4}+{10,20,30,40}={11,22,33,44}; extract lane0 via cvtt → 11.
# movmskps of {-1,2,-3,4} → sign bits 0b0101 = 5. comiss 3<5 sets CF.
# EXPECT: exit=hlt eax=11 ebx=5 ecx=1
	.intel_syntax noprefix
	.text
	# build {1,2,3,4} at [0x8000] and {10,20,30,40} at [0x8010]
	mov eax, 1
	cvtsi2ss xmm0, eax
	movss [0x8000], xmm0
	mov eax, 2
	cvtsi2ss xmm0, eax
	movss [0x8004], xmm0
	mov eax, 3
	cvtsi2ss xmm0, eax
	movss [0x8008], xmm0
	mov eax, 4
	cvtsi2ss xmm0, eax
	movss [0x800c], xmm0
	mov eax, 10
	cvtsi2ss xmm0, eax
	movss [0x8010], xmm0
	mov eax, 20
	cvtsi2ss xmm0, eax
	movss [0x8014], xmm0
	mov eax, 30
	cvtsi2ss xmm0, eax
	movss [0x8018], xmm0
	mov eax, 40
	cvtsi2ss xmm0, eax
	movss [0x801c], xmm0
	movups xmm1, [0x8000]     # {1,2,3,4}
	movaps xmm2, xmm1
	addps xmm2, [0x8010]      # {11,22,33,44}
	cvttss2si eax, xmm2       # lane 0 = 11
	# movmskps of {-1,2,-3,4}
	mov edx, -1
	cvtsi2ss xmm3, edx
	movss [0x8020], xmm3
	mov edx, 2
	cvtsi2ss xmm3, edx
	movss [0x8024], xmm3
	mov edx, -3
	cvtsi2ss xmm3, edx
	movss [0x8028], xmm3
	mov edx, 4
	cvtsi2ss xmm3, edx
	movss [0x802c], xmm3
	movups xmm4, [0x8020]
	movmskps ebx, xmm4        # 0b0101 = 5
	# comiss: 3.0 vs 5.0 → CF set (less). setc cl → ecx bit0
	mov edx, 3
	cvtsi2ss xmm5, edx
	mov edx, 5
	cvtsi2ss xmm6, edx
	comiss xmm5, xmm6         # 3 < 5 → CF=1
	setc cl
	movzx ecx, cl             # 1
	hlt
