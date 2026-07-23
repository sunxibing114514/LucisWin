/* paint_hook.h — Win32 窗口绘制钩子接口 (Phase 2.2 / Phase 3.1 扩展)
 *
 * 测试与 iOS 宿主通过此钩子获取绘制命令。
 * EndPaint 时调用, 传命令数组, 宿主决定如何展示
 * (测试断言 / iOS 用 UIView+CGContext 渲染)。
 *
 * Phase 3.1 扩展: 命令增加 kind 字段 (TEXT/RECTANGLE/LINE),
 * 并携带颜色 (color/fill_color) 与几何坐标 (x2/y2)。
 */
#ifndef WINE_PAINT_HOOK_H
#define WINE_PAINT_HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单条绘制命令类型 */
typedef enum {
    PAINT_TEXT = 1,        /* 文本输出 (TextOutW/ExtTextOutW) */
    PAINT_RECTANGLE,       /* 矩形 (Rectangle) */
    PAINT_LINE,            /* 直线 (LineTo) */
} paint_kind_t;

/* 单条绘制命令 */
typedef struct {
    int           used;
    paint_kind_t  kind;       /* 命令类型; 旧路径默认 PAINT_TEXT */
    int           x, y;       /* TEXT: 文本起点; RECT: 左上; LINE: 起点 */
    int           x2, y2;     /* RECT: 右下; LINE: 终点; TEXT: 未用 */
    uint32_t      color;      /* 前景色 (笔/文字色, BGR) */
    uint32_t      fill_color; /* RECT 填充色 (刷); 其它未用 */
    uint16_t      text[128];  /* UTF-16, 以 \0 结尾 (仅 TEXT) */
} wine_paint_cmd_t;

/* 绘制钩子: EndPaint 时调用, cmds 数组 + 数量 */
typedef void (*wine_paint_hook_t)(const wine_paint_cmd_t *cmds, int count);

/* 安装绘制钩子 (全局, 后装覆盖先装)。NULL=不回调 */
void wine_set_paint_hook(wine_paint_hook_t hook);

/* 重置窗口管理器状态 (测试间清状态用) */
void wine_window_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WINE_PAINT_HOOK_H */
