/* msvcrt.c — MSVCRT DLL 实现 (Phase 1: atexit 空实现)
 *
 * hello.exe 用 -nostartfiles 编译, 但 mingw 仍可能导入 atexit。
 * Phase 1 不支持 atexit 回调注册, thunk 返回 0。
 */
#include "wine/cpu.h"

void msvcrt_atexit_thunk(cpu_context_t *ctx) {
    /* [ESP+4] = 函数指针 (Phase 1 忽略) */
    ctx->gpr[CPU_REG_EAX] = 0;
}
