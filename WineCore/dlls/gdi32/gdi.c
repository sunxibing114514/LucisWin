/* gdi.c — Phase 3.1 GDI 对象模型 + gdi32 thunk 实现
 *
 * 维护 GDI 对象表 (PEN/BRUSH/FONT/DC) 与 DC 状态。
 * 所有 thunk 按 __stdcall 栈布局读参数 ([ESP]=ret, [ESP+4]=arg1, ...),
 * 返回值写 EAX。
 *
 * paint_hook 集成: TextOutW/Rectangle/LineTo 把命令追加到当前 paint 的
 * g_paint_cmds 数组, EndPaint 时由 window.c 调钩子回放。
 */
#include "wine/cpu.h"
#include "wine/gdi.h"
#include "wine/paint_hook.h"
#include "wine/win32_types.h"
#include <string.h>

/* ---- 对象表 ---- */
static gdi_obj_t g_objs[WINE_GDI_OBJ_MAX];

void wine_gdi_reset(void) {
    memset(g_objs, 0, sizeof(g_objs));
}

int wine_gdi_obj_count(void) {
    int n = 0;
    for (int i = 0; i < WINE_GDI_OBJ_MAX; i++) if (g_objs[i].used) n++;
    return n;
}

gdi_obj_t *wine_gdi_obj_get(uint32_t handle) {
    if (handle == 0 || handle > WINE_GDI_OBJ_MAX) return NULL;
    gdi_obj_t *o = &g_objs[handle - 1];
    return o->used ? o : NULL;
}

/* 内部分配: 找空槽, 返回 1-based handle (0=表满) */
static uint32_t gdi_alloc(gdi_obj_kind_t kind) {
    for (int i = 0; i < WINE_GDI_OBJ_MAX; i++) {
        if (!g_objs[i].used) {
            memset(&g_objs[i], 0, sizeof(g_objs[i]));
            g_objs[i].used = 1;
            g_objs[i].kind = kind;
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

/* ---- 绘制命令缓冲 (与 window.c 共享) ----
 * window.c 持有 g_paint_cmds/g_paint_cmd_count, 这里 extern 引用以便
 * TextOutW/Rectangle/LineTo 追加命令。 */
extern wine_paint_cmd_t g_paint_cmds[];
extern int g_paint_cmd_count;
extern uint32_t g_paint_hdc;
#define WINE_PAINT_CMD_MAX 16

/* 按 hdc 取 DC 对象; hdc 为伪 0 或无效时返回 NULL */
static gdi_obj_t *dc_get(uint32_t hdc) {
    gdi_obj_t *o = wine_gdi_obj_get(hdc);
    return (o && o->kind == GDI_OBJ_DC) ? o : NULL;
}

/* ============================================================
 * 对象创建 thunk
 * ============================================================ */

/* CreatePen(fnPenStyle, nWidth, crColor) -> HGDIOBJ */
void gdi32_CreatePen_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    int32_t  style = (int32_t)cpu_mem_r32(ctx, esp + 4);
    int32_t  width = (int32_t)cpu_mem_r32(ctx, esp + 8);
    uint32_t color = cpu_mem_r32(ctx, esp + 12);
    uint32_t h = gdi_alloc(GDI_OBJ_PEN);
    if (!h) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    gdi_obj_t *o = wine_gdi_obj_get(h);
    o->pen_style = style;
    o->pen_width = width;
    o->pen_color = color;
    ctx->gpr[CPU_REG_EAX] = h;
}

/* CreateSolidBrush(crColor) -> HBRUSH */
void gdi32_CreateSolidBrush_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t color = cpu_mem_r32(ctx, esp + 4);
    uint32_t h = gdi_alloc(GDI_OBJ_BRUSH);
    if (!h) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    gdi_obj_t *o = wine_gdi_obj_get(h);
    o->brush_style = 0; /* BS_SOLID */
    o->brush_color = color;
    ctx->gpr[CPU_REG_EAX] = h;
}

/* CreateFontIndirectW(lplf) -> HFONT
 * LOGFONTW 布局 (节选): lfHeight@0, lfFaceName@28 (UTF-16, 32 chars) */
void gdi32_CreateFontIndirectW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t lf_ga = cpu_mem_r32(ctx, esp + 4);
    if (!lf_ga) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    uint8_t *p = (uint8_t *)ctx->mem_base + lf_ga;
    int32_t height = (int32_t)cpu_mem_r32(ctx, lf_ga);
    const uint16_t *face = (const uint16_t *)(p + 28);

    uint32_t h = gdi_alloc(GDI_OBJ_FONT);
    if (!h) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    gdi_obj_t *o = wine_gdi_obj_get(h);
    o->font_height = height;
    int i = 0;
    for (; i < 31 && face[i]; i++) o->font_face[i] = (char)(face[i] & 0xFF);
    o->font_face[i] = 0;
    ctx->gpr[CPU_REG_EAX] = h;
}

/* ============================================================
 * SelectObject / DeleteObject
 * ============================================================ */

/* SelectObject(hdc, hgdiobj) -> 旧对象 handle (0=无) */
void gdi32_SelectObject_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    uint32_t hobj = cpu_mem_r32(ctx, esp + 8);

    gdi_obj_t *dc = dc_get(hdc);
    gdi_obj_t *obj = wine_gdi_obj_get(hobj);
    if (!dc || !obj) { ctx->gpr[CPU_REG_EAX] = 0; return; }

    uint32_t old = 0;
    switch (obj->kind) {
    case GDI_OBJ_PEN:
        old = dc->dc_pen; dc->dc_pen = hobj; break;
    case GDI_OBJ_BRUSH:
        old = dc->dc_brush; dc->dc_brush = hobj; break;
    case GDI_OBJ_FONT:
        old = dc->dc_font; dc->dc_font = hobj; break;
    default:
        old = 0;
    }
    ctx->gpr[CPU_REG_EAX] = old;
}

/* DeleteObject(hgdiobj) -> BOOL (1=成功) */
void gdi32_DeleteObject_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hobj = cpu_mem_r32(ctx, esp + 4);
    gdi_obj_t *o = wine_gdi_obj_get(hobj);
    if (!o || o->kind == GDI_OBJ_DC) {
        /* 不删 DC (DC 由 EndPaint 管); 其它无效返回 0 */
        ctx->gpr[CPU_REG_EAX] = 0;
        return;
    }
    o->used = 0;
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* ============================================================
 * 几何绘制
 * ============================================================ */

/* MoveToEx(hdc, x, y, lpPoint) -> BOOL
 * lpPoint 非 NULL 时写入旧位置 (POINT = {x,y} 8 字节) */
void gdi32_MoveToEx_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    int32_t  x = (int32_t)cpu_mem_r32(ctx, esp + 8);
    int32_t  y = (int32_t)cpu_mem_r32(ctx, esp + 12);
    uint32_t lppt = cpu_mem_r32(ctx, esp + 16);

    gdi_obj_t *dc = dc_get(hdc);
    if (!dc) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    if (lppt) {
        uint8_t *p = (uint8_t *)ctx->mem_base + lppt;
        memcpy(p, &dc->dc_cur_x, 4);
        memcpy(p + 4, &dc->dc_cur_y, 4);
    }
    dc->dc_cur_x = x;
    dc->dc_cur_y = y;
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* LineTo(hdc, x, y) -> BOOL
 * 记录 LINE 命令 (cur→x,y), 更新 cur=x,y */
void gdi32_LineTo_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    int32_t  x = (int32_t)cpu_mem_r32(ctx, esp + 8);
    int32_t  y = (int32_t)cpu_mem_r32(ctx, esp + 12);

    gdi_obj_t *dc = dc_get(hdc);
    if (!dc) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    if (g_paint_cmd_count < WINE_PAINT_CMD_MAX && g_paint_hdc == hdc) {
        wine_paint_cmd_t *c = &g_paint_cmds[g_paint_cmd_count++];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->kind = PAINT_LINE;
        c->x = dc->dc_cur_x;
        c->y = dc->dc_cur_y;
        c->x2 = x;
        c->y2 = y;
        c->color = dc->dc_pen ? wine_gdi_obj_get(dc->dc_pen)->pen_color : 0;
    }
    dc->dc_cur_x = x;
    dc->dc_cur_y = y;
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* Rectangle(hdc, left, top, right, bottom) -> BOOL
 * 记录 RECTANGLE 命令 (边框色=dc_pen, 填充色=dc_brush) */
void gdi32_Rectangle_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    int32_t  l = (int32_t)cpu_mem_r32(ctx, esp + 8);
    int32_t  t = (int32_t)cpu_mem_r32(ctx, esp + 12);
    int32_t  r = (int32_t)cpu_mem_r32(ctx, esp + 16);
    int32_t  b = (int32_t)cpu_mem_r32(ctx, esp + 20);

    gdi_obj_t *dc = dc_get(hdc);
    if (!dc) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    if (g_paint_cmd_count < WINE_PAINT_CMD_MAX && g_paint_hdc == hdc) {
        wine_paint_cmd_t *c = &g_paint_cmds[g_paint_cmd_count++];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->kind = PAINT_RECTANGLE;
        c->x = l; c->y = t;
        c->x2 = r; c->y2 = b;
        c->color = dc->dc_pen ? wine_gdi_obj_get(dc->dc_pen)->pen_color : 0;
        c->fill_color = dc->dc_brush ? wine_gdi_obj_get(dc->dc_brush)->brush_color : 0xFFFFFF;
    }
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* ============================================================
 * 文本绘制
 * ============================================================ */

/* ExtTextOutW(hdc, x, y, fuOptions, lprc, lpString, cbCount, lpDx) -> BOOL
 * Phase 3.1: 忽略 fuOptions/lprc/lpDx, 仅记录文本与 dc_text_color */
void gdi32_ExtTextOutW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    int32_t  x = (int32_t)cpu_mem_r32(ctx, esp + 8);
    int32_t  y = (int32_t)cpu_mem_r32(ctx, esp + 12);
    /* esp+16 fuOptions, esp+20 lprc */
    uint32_t str_ga = cpu_mem_r32(ctx, esp + 24);
    int32_t  cch = (int32_t)cpu_mem_r32(ctx, esp + 28);

    gdi_obj_t *dc = dc_get(hdc);
    if (!dc || g_paint_cmd_count >= WINE_PAINT_CMD_MAX || g_paint_hdc != hdc) {
        ctx->gpr[CPU_REG_EAX] = 0;
        return;
    }
    wine_paint_cmd_t *c = &g_paint_cmds[g_paint_cmd_count++];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->kind = PAINT_TEXT;
    c->x = x; c->y = y;
    c->color = dc->dc_text_color;
    const uint16_t *s = (const uint16_t *)((uint8_t *)ctx->mem_base + str_ga);
    int n = cch;
    if (n > 127) n = 127;
    int i;
    for (i = 0; i < n; i++) c->text[i] = s[i];
    c->text[n] = 0;
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* TextOutW(hdc, x, y, lpString, cbCount) -> BOOL
 * 转发 ExtTextOut 路径 */
void gdi32_TextOutW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    int32_t  x = (int32_t)cpu_mem_r32(ctx, esp + 8);
    int32_t  y = (int32_t)cpu_mem_r32(ctx, esp + 12);
    uint32_t str_ga = cpu_mem_r32(ctx, esp + 16);
    int32_t  cch = (int32_t)cpu_mem_r32(ctx, esp + 20);

    gdi_obj_t *dc = dc_get(hdc);
    if (!dc || g_paint_cmd_count >= WINE_PAINT_CMD_MAX || g_paint_hdc != hdc) {
        ctx->gpr[CPU_REG_EAX] = 0;
        return;
    }
    wine_paint_cmd_t *c = &g_paint_cmds[g_paint_cmd_count++];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->kind = PAINT_TEXT;
    c->x = x; c->y = y;
    c->color = dc->dc_text_color;
    const uint16_t *s = (const uint16_t *)((uint8_t *)ctx->mem_base + str_ga);
    int n = cch;
    if (n > 127) n = 127;
    int i;
    for (i = 0; i < n; i++) c->text[i] = s[i];
    c->text[n] = 0;
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* ============================================================
 * DC 状态
 * ============================================================ */

/* SetTextColor(hdc, crColor) -> 旧颜色 */
void gdi32_SetTextColor_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    uint32_t color = cpu_mem_r32(ctx, esp + 8);
    gdi_obj_t *dc = dc_get(hdc);
    if (!dc) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    uint32_t old = dc->dc_text_color;
    dc->dc_text_color = color;
    ctx->gpr[CPU_REG_EAX] = old;
}

/* SetBkMode(hdc, iBkMode) -> 旧模式 */
void gdi32_SetBkMode_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    int32_t  mode = (int32_t)cpu_mem_r32(ctx, esp + 8);
    gdi_obj_t *dc = dc_get(hdc);
    if (!dc) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    int32_t old = dc->dc_bk_mode;
    dc->dc_bk_mode = mode;
    ctx->gpr[CPU_REG_EAX] = (uint32_t)old;
}

/* SetBkColor(hdc, crColor) -> 旧颜色 */
void gdi32_SetBkColor_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hdc = cpu_mem_r32(ctx, esp + 4);
    uint32_t color = cpu_mem_r32(ctx, esp + 8);
    gdi_obj_t *dc = dc_get(hdc);
    if (!dc) { ctx->gpr[CPU_REG_EAX] = 0; return; }
    uint32_t old = dc->dc_bk_color;
    dc->dc_bk_color = color;
    ctx->gpr[CPU_REG_EAX] = old;
}

/* GetDeviceCaps(hdc, nIndex) -> 值
 * 硬编码: LOGPIXELSX=88->96, LOGPIXELSY=90->96, HORZRES=8->400, VERTRES=10->200 */
void gdi32_GetDeviceCaps_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    int32_t  idx = (int32_t)cpu_mem_r32(ctx, esp + 8);
    int32_t v = 0;
    switch (idx) {
    case 88: v = 96; break;  /* LOGPIXELSX */
    case 90: v = 96; break;  /* LOGPIXELSY */
    case 8:  v = 400; break; /* HORZRES */
    case 10: v = 200; break; /* VERTRES */
    default: v = 0;
    }
    ctx->gpr[CPU_REG_EAX] = (uint32_t)v;
}

/* ============================================================
 * 内部接口: window.c 调用分配/释放 DC
 * ============================================================ */

/* wine_gdi_alloc_dc: BeginPaint 调用, 分配 DC 并初始化默认状态
 *   返回 DC handle (0=表满) */
uint32_t wine_gdi_alloc_dc(void) {
    uint32_t h = gdi_alloc(GDI_OBJ_DC);
    if (!h) return 0;
    gdi_obj_t *dc = wine_gdi_obj_get(h);
    dc->dc_text_color = 0;          /* 黑 */
    dc->dc_bk_color = 0xFFFFFF;     /* 白 */
    dc->dc_bk_mode = 2;             /* OPAQUE */
    dc->dc_cur_x = 0;
    dc->dc_cur_y = 0;
    return h;
}

/* wine_gdi_free_dc: EndPaint 调用, 释放 DC 对象 */
void wine_gdi_free_dc(uint32_t hdc) {
    gdi_obj_t *o = wine_gdi_obj_get(hdc);
    if (o && o->kind == GDI_OBJ_DC) o->used = 0;
}
