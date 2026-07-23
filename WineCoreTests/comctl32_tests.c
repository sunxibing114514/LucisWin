/* comctl32_tests.c — Phase 3.3 单元测试: comctl32 + Edit 控件
 *
 * 直接调用 thunk (构造 cpu_context_t 模拟 __stdcall 栈), 验证:
 *   - comctl32 InitCommonControls / InitCommonControlsEx
 *   - Edit 系统类预注册 (CreateWindowExW("Edit") 返回非 0)
 *   - SetWindowTextW / GetWindowTextW / GetWindowTextLengthW 文本读写
 *   - SetWindowTextW(Edit) 向父窗口投递 WM_COMMAND(EN_CHANGE)
 *   - BeginPaint(parent) 为可见 Edit 子窗口发出 PAINT_EDIT 命令
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/cpu.h"
#include "wine/win32_types.h"
#include "wine/pe_loader.h"
#include "../WineCore/include/wine/paint_hook.h"
#include <stdint.h>
#include <string.h>

/* ---- thunk 声明 ---- */
extern void user32_RegisterClassExW_thunk(cpu_context_t *ctx);
extern void user32_CreateWindowExW_thunk(cpu_context_t *ctx);
extern void user32_SetWindowTextW_thunk(cpu_context_t *ctx);
extern void user32_GetWindowTextW_thunk(cpu_context_t *ctx);
extern void user32_GetWindowTextLengthW_thunk(cpu_context_t *ctx);
extern void user32_BeginPaint_thunk(cpu_context_t *ctx);
extern void user32_GetMessageW_thunk(cpu_context_t *ctx);
extern void comctl32_InitCommonControls_thunk(cpu_context_t *ctx);
extern void comctl32_InitCommonControlsEx_thunk(cpu_context_t *ctx);
extern void wine_window_reset(void);

/* ---- paint 命令缓冲 (window.c 中非 static, 供测试 extern) ---- */
extern wine_paint_cmd_t g_paint_cmds[];
extern int g_paint_cmd_count;

/* ---- 夹具 ---- */
#define TEST_MEM_SIZE 16384
static uint8_t g_mem[TEST_MEM_SIZE];
static pe_image_t g_dummy_img = {0};

static cpu_context_t make_ctx(void) {
    cpu_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    memset(g_mem, 0, sizeof(g_mem));
    ctx.mem_base = g_mem;
    ctx.mem_size = TEST_MEM_SIZE;
    ctx.current_image = &g_dummy_img;
    ctx.gpr[CPU_REG_ESP] = TEST_MEM_SIZE - 512;
    cpu_mem_w32(&ctx, ctx.gpr[CPU_REG_ESP], 0);  /* [ESP] = ret_addr */
    return ctx;
}

static void set_arg(cpu_context_t *ctx, int n, uint32_t val) {
    cpu_mem_w32(ctx, ctx->gpr[CPU_REG_ESP] + 4 * n, val);
}

/* 在 guest 内存 ga 处写入 ASCII→UTF-16 字符串, 返回 ga */
static uint32_t write_u16(cpu_context_t *ctx, uint32_t ga, const char *ascii) {
    uint16_t *p = (uint16_t *)((uint8_t *)ctx->mem_base + ga);
    int i;
    for (i = 0; ascii[i]; i++) p[i] = (uint16_t)ascii[i];
    p[i] = 0;
    return ga;
}

/* 注册测试类, 返回 ATOM (0=失败) */
static uint32_t register_class(cpu_context_t *ctx, const char *name) {
    uint32_t wc_ga = 0x100;
    uint32_t name_ga = 0x200;
    memset((uint8_t *)ctx->mem_base + wc_ga, 0, 72);
    cpu_mem_w32(ctx, wc_ga + 0, 72);              /* cbSize */
    cpu_mem_w32(ctx, wc_ga + 8, 0xDEADBEEF);      /* lpfnWndProc 哨兵 */
    cpu_mem_w32(ctx, wc_ga + 40, name_ga);        /* lpszClassName */
    write_u16(ctx, name_ga, name);
    set_arg(ctx, 1, wc_ga);
    user32_RegisterClassExW_thunk(ctx);
    return ctx->gpr[CPU_REG_EAX];
}

/* 创建窗口, 返回 HWND (0=失败) */
static uint32_t create_window(cpu_context_t *ctx, const char *class_name,
                              const char *window_name, uint32_t style,
                              uint32_t ex_style, uint32_t parent, uint32_t menu) {
    uint32_t cn_ga = 0x300;
    uint32_t wn_ga = window_name ? 0x400 : 0;
    write_u16(ctx, cn_ga, class_name);
    if (window_name) write_u16(ctx, wn_ga, window_name);
    set_arg(ctx, 1, ex_style);
    set_arg(ctx, 2, cn_ga);
    set_arg(ctx, 3, wn_ga);
    set_arg(ctx, 4, style);
    set_arg(ctx, 5, 10);   /* x */
    set_arg(ctx, 6, 20);   /* y */
    set_arg(ctx, 7, 100); /* w */
    set_arg(ctx, 8, 30);   /* h */
    set_arg(ctx, 9, parent);
    set_arg(ctx, 10, menu);
    set_arg(ctx, 11, 0);   /* hInstance */
    set_arg(ctx, 12, 0);   /* lpParam */
    user32_CreateWindowExW_thunk(ctx);
    return ctx->gpr[CPU_REG_EAX];
}

static void set_window_text(cpu_context_t *ctx, uint32_t hwnd, const char *text) {
    uint32_t text_ga = 0x500;
    write_u16(ctx, text_ga, text);
    set_arg(ctx, 1, hwnd);
    set_arg(ctx, 2, text_ga);
    user32_SetWindowTextW_thunk(ctx);
}

static int get_window_text(cpu_context_t *ctx, uint32_t hwnd, char *out, int max) {
    uint32_t buf_ga = 0x600;
    set_arg(ctx, 1, hwnd);
    set_arg(ctx, 2, buf_ga);
    set_arg(ctx, 3, (uint32_t)max);
    user32_GetWindowTextW_thunk(ctx);
    int n = (int)ctx->gpr[CPU_REG_EAX];
    uint16_t *p = (uint16_t *)((uint8_t *)ctx->mem_base + buf_ga);
    for (int i = 0; i < n && i < max - 1; i++) out[i] = (char)(p[i] & 0xFF);
    out[n < max - 1 ? n : max - 1] = 0;
    return n;
}

/* 从某窗口队列取一条消息 (调用 GetMessageW), 写到 msg_ga */
static int get_msg(cpu_context_t *ctx, uint32_t msg_ga) {
    set_arg(ctx, 1, msg_ga);
    set_arg(ctx, 2, 0);
    set_arg(ctx, 3, 0);
    set_arg(ctx, 4, 0);
    user32_GetMessageW_thunk(ctx);
    return (int)ctx->gpr[CPU_REG_EAX];
}

/* ---- 测试用例 ---- */

TEST(comctl32_init_returns_no_error) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    comctl32_InitCommonControls_thunk(&ctx);
    ASSERT(1, "InitCommonControls 不应崩溃");
}

TEST(comctl32_initex_returns_true) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    set_arg(&ctx, 1, 0);
    comctl32_InitCommonControlsEx_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 1,
           "InitCommonControlsEx 应返回 TRUE, 实际 %#x", ctx.gpr[CPU_REG_EAX]);
}

TEST(create_edit_window_returns_nonzero) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    uint32_t h = create_window(&ctx, "Edit", "hello",
                               WS_CHILD | WS_VISIBLE, 0, 0, 100);
    ASSERT(h != 0, "CreateWindowExW(Edit) 应返回非 0, 实际 %#x", h);
}

TEST(set_get_window_text_roundtrip) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    uint32_t h = create_window(&ctx, "Edit", "init", WS_CHILD, 0, 0, 100);
    ASSERT(h != 0, "创建 Edit 失败");
    set_window_text(&ctx, h, "hello world");
    char buf[64];
    int n = get_window_text(&ctx, h, buf, sizeof(buf));
    ASSERT(n == 11, "GetWindowTextW 应返回 11, 实际 %d", n);
    ASSERT(strcmp(buf, "hello world") == 0,
           "文本应为 'hello world', 实际 '%s'", buf);
}

TEST(get_window_text_length) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    uint32_t h = create_window(&ctx, "Edit", "abcde", WS_CHILD, 0, 0, 100);
    set_arg(&ctx, 1, h);
    user32_GetWindowTextLengthW_thunk(&ctx);
    ASSERT((int)ctx.gpr[CPU_REG_EAX] == 5,
           "长度应为 5, 实际 %d", (int)ctx.gpr[CPU_REG_EAX]);
}

TEST(set_text_on_edit_posts_wm_command_to_parent) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    register_class(&ctx, "ParentClass");
    uint32_t parent = create_window(&ctx, "ParentClass", NULL, 0, 0, 0, 0);
    ASSERT(parent != 0, "创建父窗口失败");

    /* 排空父窗口创建时投递的 WM_PAINT (顶层窗口才有, 子窗口无) */
    uint32_t msg_ga = 0x700;
    get_msg(&ctx, msg_ga);

    /* 创建 Edit 子窗口, parent=parent, hMenu=1001 */
    uint32_t edit = create_window(&ctx, "Edit", "",
                                  WS_CHILD | WS_VISIBLE, 0, parent, 1001);
    ASSERT(edit != 0, "创建 Edit 失败");

    /* SetWindowText 触发 WM_COMMAND(EN_CHANGE) 投递到父队列 */
    set_window_text(&ctx, edit, "changed");

    int got = get_msg(&ctx, msg_ga);
    ASSERT(got == 1, "GetMessageW 应返回 1, 实际 %d", got);
    win32_msg_t *m = (win32_msg_t *)((uint8_t *)ctx.mem_base + msg_ga);
    ASSERT(m->message == WM_COMMAND,
           "应为 WM_COMMAND, 实际 %#x", m->message);
    ASSERT((m->wParam & 0xFFFF) == 1001,
           "LOWORD(wParam) 应为 1001, 实际 %#x", m->wParam & 0xFFFF);
    ASSERT(((m->wParam >> 16) & 0xFFFF) == EN_CHANGE,
           "HIWORD(wParam) 应为 EN_CHANGE, 实际 %#x",
           (m->wParam >> 16) & 0xFFFF);
    ASSERT(m->lParam == edit,
           "lParam 应为 edit HWND, 实际 %#x", m->lParam);
}

TEST(beginpaint_emits_paint_edit_for_edit_child) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    register_class(&ctx, "ParentClass");
    uint32_t parent = create_window(&ctx, "ParentClass", NULL, 0, 0, 0, 0);
    uint32_t edit = create_window(&ctx, "Edit", "edit text",
                                  WS_CHILD | WS_VISIBLE, 0, parent, 100);
    (void)edit;

    /* BeginPaint(parent) 应在 g_paint_cmds 中追加 PAINT_EDIT */
    uint32_t ps_ga = 0x800;
    set_arg(&ctx, 1, parent);
    set_arg(&ctx, 2, ps_ga);
    user32_BeginPaint_thunk(&ctx);

    ASSERT(g_paint_cmd_count >= 1,
           "应有 >=1 条 paint 命令, 实际 %d", g_paint_cmd_count);
    ASSERT(g_paint_cmds[0].kind == PAINT_EDIT,
           "首条应为 PAINT_EDIT, 实际 %d", g_paint_cmds[0].kind);
    /* 文本应是 "edit text" */
    ASSERT(g_paint_cmds[0].text[0] == 'e',
           "首字符应为 'e', 实际 %04x", g_paint_cmds[0].text[0]);
    ASSERT(g_paint_cmds[0].text[4] == ' ',
           "第 5 字符应为 ' ', 实际 %04x", g_paint_cmds[0].text[4]);
}

TEST(edit_window_stores_parent_and_menu) {
    wine_window_reset();
    cpu_context_t ctx = make_ctx();
    register_class(&ctx, "ParentClass");
    uint32_t parent = create_window(&ctx, "ParentClass", NULL, 0, 0, 0, 0);
    uint32_t edit = create_window(&ctx, "Edit", "",
                                  WS_CHILD | WS_VISIBLE, 0, parent, 12345);
    ASSERT(edit != 0, "Edit 创建失败");

    /* 排空父窗口 WM_PAINT */
    uint32_t msg_ga = 0x700;
    get_msg(&ctx, msg_ga);

    /* SetWindowText 触发 WM_COMMAND, LOWORD(wParam) = control_id = menu */
    set_window_text(&ctx, edit, "x");
    get_msg(&ctx, msg_ga);
    win32_msg_t *m = (win32_msg_t *)((uint8_t *)ctx.mem_base + msg_ga);
    ASSERT(m->message == WM_COMMAND, "应为 WM_COMMAND");
    ASSERT((m->wParam & 0xFFFF) == 12345,
           "control_id 应为 12345, 实际 %#x", m->wParam & 0xFFFF);
}
