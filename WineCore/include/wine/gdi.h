/* gdi.h — GDI 对象模型公开接口 (Phase 3.1)
 *
 * 维护 GDI 对象表 (PEN/BRUSH/FONT/DC) 与 DC 状态,
 * 供 gdi32 thunk 与测试访问。
 *
 * 句柄 = 槽位 + 1 (1-based, 0=NULL/失败)。
 */
#ifndef WINE_GDI_H
#define WINE_GDI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GDI 对象类型 */
typedef enum {
    GDI_OBJ_NONE = 0,
    GDI_OBJ_PEN  = 1,
    GDI_OBJ_BRUSH = 2,
    GDI_OBJ_FONT = 3,
    GDI_OBJ_DC   = 4,
} gdi_obj_kind_t;

/* GDI 对象 (统一结构, 按 kind 用不同字段) */
typedef struct {
    int            used;
    gdi_obj_kind_t kind;
    /* PEN */
    uint32_t pen_color;    /* COLORREF (BGR) */
    int32_t  pen_style;    /* PS_SOLID=0 等 */
    int32_t  pen_width;
    /* BRUSH */
    uint32_t brush_color;
    int32_t  brush_style;  /* BS_SOLID=0 */
    /* FONT */
    char     font_face[32]; /* lfFaceName, 窄字节 (UTF-16 低字节) */
    int32_t  font_height;
    /* DC 状态 */
    uint32_t dc_pen;       /* 当前选中 pen obj id, 0=默认 (黑笔 1px) */
    uint32_t dc_brush;
    uint32_t dc_font;
    uint32_t dc_text_color; /* 默认 0 (黑) */
    uint32_t dc_bk_color;   /* 默认 0xFFFFFF (白) */
    int32_t  dc_bk_mode;    /* TRANSPARENT=1 / OPAQUE=2, 默认 OPAQUE */
    int32_t  dc_cur_x, dc_cur_y;  /* MoveToEx/LineTo 当前位置 */
} gdi_obj_t;

#define WINE_GDI_OBJ_MAX 64

/* wine_gdi_reset: 清空对象表 (测试间隔离) */
void wine_gdi_reset(void);

/* wine_gdi_obj_count: 当前 used 对象数 (测试断言用) */
int wine_gdi_obj_count(void);

/* wine_gdi_obj_get: 按 handle 取对象指针 (越界/未用返回 NULL) */
gdi_obj_t *wine_gdi_obj_get(uint32_t handle);

/* ---- DC 分配 (window.c BeginPaint/EndPaint 调用) ---- */
uint32_t wine_gdi_alloc_dc(void);
void     wine_gdi_free_dc(uint32_t hdc);

#ifdef __cplusplus
}
#endif

#endif /* WINE_GDI_H */
