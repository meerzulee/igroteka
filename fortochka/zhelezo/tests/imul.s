# EXPECT: exit=hlt eax=0xffffffeb ebx=0x0 ecx=0x1 of=1
	.intel_syntax noprefix
	.text
	mov eax, -3
	imul eax, eax, 7        # -21, no overflow: OF=0
	jo fail
	mov ebx, 0x10000
	imul ebx, ebx           # 2^32 truncates to 0: OF=CF=1
	mov ecx, 1
	jo done
fail:
	mov ecx, 0xdead
done:
	hlt
