# Calling into the hostcall window exits the run loop — the import-thunk seam.
# EXPECT: exit=hostcall eip=0xf0000010
	.intel_syntax noprefix
	.text
	mov eax, 0xF0000010
	call eax
	hlt
