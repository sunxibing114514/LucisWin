/* gdi_tests.c — Phase 3.1 单元测试: GDI 对象模型
 *
 * 直接调用 gdi32 thunk (构造 cpu_context_t 模拟 __stdcall 栈),
 * 断言对象表状态与 DC 状态变化。
 *
 * thunk 入口栈布局: [ESP]=ret_addr, [ESP+4]=arg1, [ESP+8]=arg2, ...
 * 返回值写 EAX。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/cpu.h"
#include "wine/gdi.h"
#include "../WineCore/include/wine/paint_hook.h"
#include <stdint.h>
#include <string.h>

/* ---- thunk 声明 (实现于 dlls/gdi32/gdi.c) ---- */
extern void gdi32_CreatePen_thunk(cpu_context_t *ctx);
extern void gdi32_CreateSolidBrush_thunk(cpu_context_t *ctx);
extern void gdi32_CreateFontIndirectW_thunk(cpu_context_t *ctx);
extern void gdi32_SelectObject_thunk(cpu_context_t *ctx);
extern void gdi32_DeleteObject_thunk(cpu_context_t *ctx);
extern void gdi32_SetTextColor_thunk(cpu_context_t *ctx);
extern void gdi32_GetDeviceCaps_thunk(cpu_context_t *ctx);
extern void gdi32_MoveToEx_thunk(cpu_context_t *ctx);

/* ---- 测试夹具: 构造 CPU 上下文 ---- */
#define TEST_MEM_SIZE 8192
static uint8_t g_mem[TEST_MEM_SIZE];

static cpu_context_t make_ctx(void) {
    cpu_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    memset(g_mem, 0, sizeof(g_mem));
    ctx.mem_base = g_mem;
    ctx.mem_size = TEST_MEM_SIZE;
    ctx.current_image = NULL;
    /* ESP 指向栈中, 预留 256 字节参数空间 */
    ctx.gpr[CPU_REG_ESP] = TEST_MEM_SIZE - 256;
    /* [ESP] = ret_addr (0) */
    cpu_mem_w32(&ctx, ctx.gpr[CPU_REG_ESP], 0);
    return ctx;
}

/* 向 ESP 栈写入第 n 个参数 (n>=1, ESP+4*n) */
static void set_arg(cpu_context_t *ctx, int n, uint32_t val) {
    cpu_mem_w32(ctx, ctx->gpr[CPU_REG_ESP] + 4 * n, val);
}

/* ---- 测试用例 ---- */

TEST(gdi_create_pen_returns_nonzero) {
    wine_gdi_reset();
    cpu_context_t ctx = make_ctx();
    /* CreatePen(PS_SOLID=0, 1, RGB(255,0,0)=0x000000FF) */
    set_arg(&ctx, 1, 0);       /* fnPenStyle */
    set_arg(&ctx, 2, 1);       /* nWidth */
    set_arg(&ctx, 3, 0x000000FF); /* crColor (红) */
    gdi32_CreatePen_thunk(&ctx);
    uint32_t h = ctx.gpr[CPU_REG_EAX];
    ASSERT(h != 0, "CreatePen 应返回非 0 handle");
    gdi_obj_t *o = wine_gdi_obj_get(h);
    ASSERT(o != NULL, "handle 应有效");
    ASSERT(o->kind == GDI_OBJ_PEN, "应为 PEN 类型, 实际 %d", o->kind);
    ASSERT(o->pen_color == 0x000000FF, "pen_color 应为红, 实际 %#x", o->pen_color);
}

TEST(gdi_create_solid_brush_returns_nonzero) {
    wine_gdi_reset();
    cpu_context_t ctx = make_ctx();
    /* CreateSolidBrush(RGB(0,0,255)=0x00FF0000) */
    set_arg(&ctx, 1, 0x00FF0000);
    gdi32_CreateSolidBrush_thunk(&ctx);
    uint32_t h = ctx.gpr[CPU_REG_EAX];
    ASSERT(h != 0, "CreateSolidBrush 应返回非 0 handle");
    gdi_obj_t *o = wine_gdi_obj_get(h);
    ASSERT(o->kind == GDI_OBJ_BRUSH, "应为 BRUSH 类型");
    ASSERT(o->brush_color == 0x00FF0000, "brush_color 应为蓝, 实际 %#x", o->brush_color);
}

TEST(gdi_select_object_records_in_dc) {
    wine_gdi_reset();
    uint32_t hdc = wine_gdi_alloc_dc();
    ASSERT(hdc != 0, "DC 分配失败");
    gdi_obj_t *dc = wine_gdi_obj_get(hdc);
    ASSERT(dc->kind == GDI_OBJ_DC, "应为 DC 类型");

    /* 创建笔 */
    cpu_context_t ctx = make_ctx();
    set_arg(&ctx, 1, 0);       /* style */
    set_arg(&ctx, 2, 1);       /* width */
    set_arg(&ctx, 3, 0x000000FF); /* color 红 */
    gdi32_CreatePen_thunk(&ctx);
    uint32_t hpen = ctx.gpr[CPU_REG_EAX];

    /* SelectObject(hdc, hpen) */
    ctx = make_ctx();
    set_arg(&ctx, 1, hdc);
    set_arg(&ctx, 2, hpen);
    gdi32_SelectObject_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0, "首次 Select 应返回旧值 0 (无先前), 实际 %#x",
           ctx.gpr[CPU_REG_EAX]);

    /* 验证 DC 记录了 pen */
    ASSERT(dc->dc_pen == hpen, "DC.dc_pen 应为 %u, 实际 %u", hpen, dc->dc_pen);
}

TEST(gdi_select_object_returns_previous) {
    wine_gdi_reset();
    uint32_t hdc = wine_gdi_alloc_dc();

    /* 第一支笔 */
    cpu_context_t ctx = make_ctx();
    set_arg(&ctx, 3, 0x000000FF);
    gdi32_CreatePen_thunk(&ctx);
    uint32_t hpen1 = ctx.gpr[CPU_REG_EAX];

    /* 第二支笔 */
    ctx = make_ctx();
    set_arg(&ctx, 3, 0x0000FF00);
    gdi32_CreatePen_thunk(&ctx);
    uint32_t hpen2 = ctx.gpr[CPU_REG_EAX];

    /* Select pen1 */
    ctx = make_ctx();
    set_arg(&ctx, 1, hdc);
    set_arg(&ctx, 2, hpen1);
    gdi32_SelectObject_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0, "首次应返回 0");

    /* Select pen2, 应返回 pen1 */
    ctx = make_ctx();
    set_arg(&ctx, 1, hdc);
    set_arg(&ctx, 2, hpen2);
    gdi32_SelectObject_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == hpen1,
           "二次 Select 应返回旧 pen handle %u, 实际 %u",
           hpen1, ctx.gpr[CPU_REG_EAX]);
}

TEST(gdi_delete_object_frees_slot) {
    wine_gdi_reset();
    cpu_context_t ctx = make_ctx();
    set_arg(&ctx, 3, 0x000000FF);
    gdi32_CreatePen_thunk(&ctx);
    uint32_t h = ctx.gpr[CPU_REG_EAX];
    ASSERT(wine_gdi_obj_get(h) != NULL, "应有效");

    /* DeleteObject(h) */
    ctx = make_ctx();
    set_arg(&ctx, 1, h);
    gdi32_DeleteObject_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 1, "DeleteObject 应返回 1");
    ASSERT(wine_gdi_obj_get(h) == NULL, "删除后应无效");
}

TEST(gdi_set_text_color_returns_old) {
    wine_gdi_reset();
    uint32_t hdc = wine_gdi_alloc_dc();
    gdi_obj_t *dc = wine_gdi_obj_get(hdc);
    ASSERT(dc->dc_text_color == 0, "初始 text_color 应为黑 0");

    /* SetTextColor(hdc, 0x0000FF00) — 绿 */
    cpu_context_t ctx = make_ctx();
    set_arg(&ctx, 1, hdc);
    set_arg(&ctx, 2, 0x0000FF00);
    gdi32_SetTextColor_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0, "首次应返回旧值 0, 实际 %#x",
           ctx.gpr[CPU_REG_EAX]);
    ASSERT(dc->dc_text_color == 0x0000FF00, "text_color 应为绿, 实际 %#x",
           dc->dc_text_color);

    /* 再设, 应返回绿 */
    ctx = make_ctx();
    set_arg(&ctx, 1, hdc);
    set_arg(&ctx, 2, 0x00FF0000);
    gdi32_SetTextColor_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0x0000FF00, "二次应返回旧绿, 实际 %#x",
           ctx.gpr[CPU_REG_EAX]);
}

TEST(gdi_get_device_caps_logpixels) {
    wine_gdi_reset();
    /* GetDeviceCaps(hdc, LOGPIXELSX=88) -> 96 */
    cpu_context_t ctx = make_ctx();
    set_arg(&ctx, 1, 1);   /* hdc (任意, GetDeviceCaps 不查 DC) */
    set_arg(&ctx, 2, 88);  /* nIndex */
    gdi32_GetDeviceCaps_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 96, "LOGPIXELSX 应为 96, 实际 %u",
           ctx.gpr[CPU_REG_EAX]);

    /* GetDeviceCaps(hdc, HORZRES=8) -> 400 */
    ctx = make_ctx();
    set_arg(&ctx, 2, 8);
    gdi32_GetDeviceCaps_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 400, "HORZRES 应为 400, 实际 %u",
           ctx.gpr[CPU_REG_EAX]);
}

TEST(gdi_movetoex_writes_old_point) {
    wine_gdi_reset();
    uint32_t hdc = wine_gdi_alloc_dc();
    gdi_obj_t *dc = wine_gdi_obj_get(hdc);
    dc->dc_cur_x = 10;  /* 预设当前点 */
    dc->dc_cur_y = 20;

    /* MoveToEx(hdc, 100, 200, lpPoint) — lpPoint 应被写为旧 (10, 20) */
    uint32_t lppt = 1024;  /* mem 中某地址 */
    cpu_context_t ctx = make_ctx();
    set_arg(&ctx, 1, hdc);
    set_arg(&ctx, 2, 100);
    set_arg(&ctx, 3, 200);
    set_arg(&ctx, 4, lppt);
    gdi32_MoveToEx_thunk(&ctx);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 1, "应返回 1");

    uint32_t old_x = cpu_mem_r32(&ctx, lppt);
    uint32_t old_y = cpu_mem_r32(&ctx, lppt + 4);
    ASSERT(old_x == 10, "旧 x 应为 10, 实际 %u", old_x);
    ASSERT(old_y == 20, "旧 y 应为 20, 实际 %u", old_y);
    ASSERT(dc->dc_cur_x == 100 && dc->dc_cur_y == 200,
           "新点应为 (100,200), 实际 (%d,%d)", dc->dc_cur_x, dc->dc_cur_y);
}
