#include "textflag.h"

// memset is the objects' memset (symbols.go): CX bytes at AX get the low
// byte of BX. It writes AX, BX, CX, DI and X0, as aot.cpp's memsetGprs and
// memsetXmms say. Lengths up to 2 KiB take stores of the byte repeated,
// overlapping at the end; longer ones REP STOSB.
TEXT ·memset(SB), NOSPLIT|NOFRAME, $0-0
	MOVBQZX	BX, BX
	MOVQ	$0x0101010101010101, DI
	IMULQ	DI, BX
	CMPQ	CX, $16
	JA	above16
	CMPQ	CX, $8
	JB	below8
	MOVQ	BX, (AX)
	MOVQ	BX, -8(AX)(CX*1)
	RET

below8:
	CMPQ	CX, $4
	JB	below4
	MOVL	BX, (AX)
	MOVL	BX, -4(AX)(CX*1)
	RET

below4:
	TESTQ	CX, CX
	JEQ	done
	MOVB	BX, (AX)
	MOVB	BX, -1(AX)(CX*1)
	CMPQ	CX, $2
	JBE	done
	MOVB	BX, 1(AX)

done:
	RET

above16:
	MOVQ	BX, X0
	PUNPCKLQDQ	X0, X0
	CMPQ	CX, $32
	JA	above32
	MOVOU	X0, (AX)
	MOVOU	X0, -16(AX)(CX*1)
	RET

above32:
	CMPQ	CX, $2048
	JAE	large
	MOVOU	X0, -32(AX)(CX*1)
	MOVOU	X0, -16(AX)(CX*1)

loop:
	MOVOU	X0, (AX)
	MOVOU	X0, 16(AX)
	ADDQ	$32, AX
	SUBQ	$32, CX
	CMPQ	CX, $32
	JA	loop
	RET

large:
	MOVQ	AX, DI
	MOVQ	BX, AX
	REP; STOSB
	RET

// func memsetCode() uintptr
TEXT ·memsetCode(SB), NOSPLIT, $0-8
	LEAQ	·memset(SB), AX
	MOVQ	AX, ret+0(FP)
	RET
