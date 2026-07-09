/* fpu.exe — real floating-point through the whole runtime. The compiler lowers
 * these float ops to x87 (fld/fmul/fadd/fistp) with -nostdlib + hardware FPU, so
 * a correct exit code proves execX87 runs under peload→zhelezo→k32web, not just
 * in the CPU unit harness. Exits (int)(6.5*4.0 + 16.0) = 42.
 */
#include <windows.h>

void start(void)
{
    volatile float a = 6.5f, b = 4.0f, c = 16.0f;
    int r = (int)(a * b + c);
    ExitProcess((UINT)r);
}
