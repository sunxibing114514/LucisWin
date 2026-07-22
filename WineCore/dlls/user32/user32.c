/* user32.c — USER32 DLL 实现 (Phase 1: MessageBoxA)
 *
 * 提供 MessageBoxA 的 thunk (解释器通过 IAT 调用) 和可插拔钩子。
 *
 * 调用链:
 *   guest: push flags; push caption; push text; push 0; call MessageBoxA
 *   interpreter: call eax → eax = WINE_FUNC_ID_BASE + id → 调用 thunk
 *   thunk: 从 guest 栈 [ESP+4..] 读参数, 转换 guest 指针 → host 指针,
 *          调用 user32_MessageBoxA_impl → cp_to_unicode → 钩子
 */
#include "messagebox_hook.h"
#include "wine/codepage.h"
#include "wine/cpu.h"
#include "wine/pe_loader.h"
#include <string.h>
#include <stdio.h>

static messagebox_hook_t g_hook = NULL;

void user32_set_messagebox_hook(messagebox_hook_t hook) {
    g_hook = hook;
}

/* 将 guest 地址转为 host 指针 (PE 镜像内) */
static const void *guest_to_host(cpu_context_t *ctx, uint32_t ga) {
    if (!ga || !ctx->current_image) return NULL;
    /* 重定位后 guest 地址 = mem_base 偏移, 直接加基址 */
    return (const void *)((uint8_t *)ctx->mem_base + ga);
}

/* MessageBoxA 实现: Shift-JIS → UTF-16, 调用钩子 */
static int user32_MessageBoxA_impl(void *hwnd, const char *text,
                                    const char *caption, uint32_t flags) {
    (void)hwnd;
    uint16_t wide_text[512];
    uint16_t wide_cap[128];
    int tlen = 0, clen = 0;

    if (text) {
        tlen = cp_to_unicode(CP_SJIS, text, strlen(text) + 1,
                             wide_text, sizeof(wide_text)/sizeof(wide_text[0]));
        if (tlen == 0) wide_text[0] = 0;
    }
    if (caption) {
        clen = cp_to_unicode(CP_SJIS, caption, strlen(caption) + 1,
                             wide_cap, sizeof(wide_cap)/sizeof(wide_cap[0]));
        if (clen == 0) wide_cap[0] = 0;
    }

    int result = 0;
    if (g_hook) {
        g_hook(text ? wide_text : NULL,
               caption ? wide_cap : NULL,
               flags, &result);
    }
    return result ? result : 1; /* IDOK */
}

/* thunk: 解释器入口 */
void user32_MessageBoxA_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    /* [ESP] = 返回地址 (由解释器压入)
     * [ESP+4]  = hwnd (NULL)
     * [ESP+8]  = text (guest 指针)
     * [ESP+12] = caption (guest 指针)
     * [ESP+16] = flags (uint32) */
    uint32_t text_ga    = cpu_mem_r32(ctx, esp + 8);
    uint32_t cap_ga     = cpu_mem_r32(ctx, esp + 12);
    uint32_t flags       = cpu_mem_r32(ctx, esp + 16);

    const char *text    = (const char *)guest_to_host(ctx, text_ga);
    const char *caption = (const char *)guest_to_host(ctx, cap_ga);

    int ret = user32_MessageBoxA_impl(NULL, text, caption, flags);
    ctx->gpr[CPU_REG_EAX] = (uint32_t)ret;
}
