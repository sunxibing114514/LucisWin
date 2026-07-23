/* paint_hook.h — Win32 窗口绘制钩子接口 (Phase 2.2)
 *
 * 测试与 iOS 宿主通过此钩子获取 TextOutW 绘制命令。
 * EndPaint 时调用, 传命令数组, 宿主决定如何展示
 * (测试断言文本 / iOS 用 UIView+CGContext 渲染)。
 */
#ifndef WINE_PAINT_HOOK_H
#define WINE_PAINT_HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单条绘制命令 (TextOutW 记录) */
typedef struct {
    int      used;
    int      x, y;
    uint16_t text[128];   /* UTF-16, 以 \0 结尾 */
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
