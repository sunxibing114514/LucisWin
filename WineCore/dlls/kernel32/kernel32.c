/* kernel32.c — KERNEL32 DLL 实现 (Phase 1: ExitProcess)
 *
 * ExitProcess thunk: 读退出码, 通过 cpu_exit_process longjmp 回 cpu_run。
 */
#include "wine/cpu.h"

void kernel32_ExitProcess_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    /* [ESP] = 返回地址, [ESP+4] = exit code */
    uint32_t code = cpu_mem_r32(ctx, esp + 4);
    cpu_exit_process(ctx, code);
}
