/* messagebox_hook.h — MessageBoxA 钩子接口
 *
 * 功能: 允许宿主 (iOS/macOS/test) 安装一个钩子, 拦截 MessageBoxA 调用,
 *       接收已转为 UTF-16 的 text/caption, 决定如何展示 (UIAlertController / printf / 断言)。
 *
 * 设计: 钩子是可插拔的, user32_MessageBoxA 实现内部调用钩子;
 *       Phase 1 测试安装捕获钩子, iOS 壳安装 UIAlertController 钩子。
 */
#ifndef WINE_MESSAGEBOX_HOOK_H
#define WINE_MESSAGEBOX_HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 钩子函数类型
 *   text/caption: UTF-16LE 以 \0 结尾的字符串 (已从 Shift-JIS 转换)
 *   flags:        MessageBoxA 的 uType 参数 (如 MB_OK=0)
 *   result:       输出参数, 设置返回值 (如 IDOK=1); NULL 表示忽略 */
typedef void (*messagebox_hook_t)(const uint16_t *text,
                                   const uint16_t *caption,
                                   uint32_t flags,
                                   int *result);

/* user32_set_messagebox_hook: 安装钩子 (全局, 后装覆盖先装)
 *   传 NULL 恢复默认行为 (无钩子时 user32_MessageBoxA 返回 IDOK) */
void user32_set_messagebox_hook(messagebox_hook_t hook);

#ifdef __cplusplus
}
#endif

#endif /* WINE_MESSAGEBOX_HOOK_H */
