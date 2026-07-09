# SSE1 scalar single: cvtsi2ss/addss/mulss/divss/sqrtss + cvttss2si round trip.
# (10.0 + 4.0) * 2.0 = 28.0 ; sqrt(144)=12 ; 100/8=12.5 → cvtt → 12
# EXPECT: exit=hlt eax=28 ebx=12 ecx=12
	.intel_syntax noprefix
	.text
	# eax = (int)(((10+4)*2))
	mov eax, 10
	cvtsi2ss xmm0, eax        # xmm0 = 10.0
	mov eax, 4
	cvtsi2ss xmm1, eax        # xmm1 = 4.0
	addss xmm0, xmm1          # 14.0
	mov eax, 2
	cvtsi2ss xmm2, eax
	mulss xmm0, xmm2          # 28.0
	cvttss2si eax, xmm0       # 28
	# ebx = (int)sqrt(144)
	mov edx, 144
	cvtsi2ss xmm3, edx
	sqrtss xmm3, xmm3         # 12.0
	cvttss2si ebx, xmm3       # 12
	# ecx = (int)(100/8) = (int)12.5 = 12 (truncate)
	mov edx, 100
	cvtsi2ss xmm4, edx
	mov edx, 8
	cvtsi2ss xmm5, edx
	divss xmm4, xmm5          # 12.5
	cvttss2si ecx, xmm4       # 12
	hlt
