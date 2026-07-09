# x87 is out of tier-0 scope for now: must fault precisely at the instruction.
# EXPECT: exit=fault fault=ud eip=0x1000
	.intel_syntax noprefix
	.text
	fld1
	hlt
