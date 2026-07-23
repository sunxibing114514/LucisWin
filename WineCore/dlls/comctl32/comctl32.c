/* comctl32.c — Phase 3.3 comctl32.dll thunk 实现
 *
 * InitCommonControls / InitCommonControlsEx: 空实现。
 * "Edit" 系统类已在 wine_window_reset() 中预注册 (user32 侧),
 * 故此处无需做实际工作 — 仅满足 PE IAT 导入解析。
 *
 * 所有 thunk 按 __stdcall 栈布局读参数 ([ESP]=ret, [ESP+4]=arg1, ...),
 * 返回值写 EAX。
 */
#include "wine/cpu.h"

/* void InitCommonControls(void) — 空实现, 返回无 (EAX 不用) */
void comctl32_InitCommonControls_thunk(cpu_context_t *ctx) {
    (void)ctx;
    /* "Edit" 已预注册, 无需工作 */
    ctx->gpr[CPU_REG_EAX] = 0;
}

/* BOOL InitCommonControlsEx(LPINITCOMMONCONTROLSEX) — 总返回 TRUE
 * 参数: [ESP+4] = lpInitCtrls (guest 指针, 本实现忽略) */
void comctl32_InitCommonControlsEx_thunk(cpu_context_t *ctx) {
    (void)ctx;
    ctx->gpr[CPU_REG_EAX] = 1;  /* TRUE */
}
