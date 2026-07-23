/* msvcrt.c — MSVCRT DLL 实现 (Phase 1: atexit / Phase 3.4: memset)
 *
 * hello.exe 用 -nostartfiles 编译, 但 mingw 仍可能导入 atexit。
 * Phase 1 不支持 atexit 回调注册, thunk 返回 0。
 * Phase 3.4: notepad.exe 的 memset(&wc, 0, sizeof) 调用走 msvcrt.memset。
 */
#include "wine/cpu.h"
#include <string.h>

void msvcrt_atexit_thunk(cpu_context_t *ctx) {
    /* [ESP+4] = 函数指针 (Phase 1 忽略) */
    ctx->gpr[CPU_REG_EAX] = 0;
}

/* memset(dst, c, n) -> dst
 * 标准签名: void *memset(void *dst, int c, size_t n); 返回 dst */
void msvcrt_memset_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t dst = cpu_mem_r32(ctx, esp + 4);
    uint32_t c   = cpu_mem_r32(ctx, esp + 8);
    uint32_t n   = cpu_mem_r32(ctx, esp + 12);
    if (dst && n) {
        memset(ctx->mem_base + dst, (int)(c & 0xFF), n);
    }
    ctx->gpr[CPU_REG_EAX] = dst;
}
