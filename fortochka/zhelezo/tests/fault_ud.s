# An architecturally-undefined opcode must fault precisely at the instruction.
# (Was fld1 before x87 landed; UD2 is guaranteed-invalid forever.)
# EXPECT: exit=fault fault=ud eip=0x1000
	.intel_syntax noprefix
	.text
	ud2
	hlt
