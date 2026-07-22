/* wine.h — luciswin 核心公开 API
 *
 * 功能: 暴露 wine_init / wine_run_exe 等入口, 供 iOS 应用壳 (WineWrapper)
 *       与测试调用; 同时提供内建 DLL 导出注册机制 (wine_builtin_register/lookup),
 *       供 pe_loader 在解析导入表时查找内建函数地址。
 *
 * Phase 1 范围:
 *   - wine_init: 注册内建 DLL (kernel32/user32/msvcrt) 的导出
 *   - wine_run_exe: 加载 PE + 跑解释器直到 ExitProcess
 *   - 内建注册表: 简单线性表, 容量足够 Phase 1
 */
#ifndef WINE_WINE_H
#define WINE_WINE_H

#include <stdint.h>
#include <stddef.h>

/* 调用约定: 在非 Windows 平台 __cdecl 未定义, 补空宏。
 * Win32 的 __cdecl 在 Linux/macOS/arm64 上等价于默认 C 调用约定,
 * 内建函数按普通 C ABI 实现, 解释器调用时无需特殊处理。 */
#ifndef __cdecl
#define __cdecl
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 内建导出项: 函数名 -> 宿主 C 函数地址 */
typedef struct {
    const char *name;   /* 导出函数名 (如 "MessageBoxA"), 不含下划线 */
    void       *addr;    /* 宿主 C 函数指针 (__cdecl) */
} wine_export_t;

/* 内建 DLL 描述: DLL 名 + 其导出表 */
typedef struct {
    const char    *dll;       /* DLL 名 (小写, 如 "user32.dll") */
    wine_export_t *exports;   /* 以 {NULL,NULL} 结尾的导出表 */
} wine_builtin_dll_t;

/* wine_init: 初始化 Wine 运行时, 注册所有内建 DLL
 *   Phase 1: 注册 kernel32/user32/msvcrt 的 Phase 1 导出子集
 *   返回: 0 成功, 非零失败 */
int wine_init(void);

/* wine_run_exe: 加载并运行一个 PE 文件
 *   data/len: PE 文件原始字节
 *   返回: 进程退出码 (ExitProcess 的参数), 负数表示加载失败 */
int wine_run_exe(const uint8_t *data, size_t len);

/* wine_builtin_register: 注册一个内建 DLL 的导出表
 *   dll:      DLL 名 (内部转小写比较, 大小写不敏感)
 *   exports:  导出表, 以 {NULL,NULL} 结尾
 *   重复注册同名 DLL 会追加其导出 */
void wine_builtin_register(const char *dll, wine_export_t *exports);

/* wine_builtin_lookup: 按函数名查找内建导出地址
 *   dll:  DLL 名 (大小写不敏感)
 *   name: 函数名 (大小写敏感, Win32 导出名区分大小写)
 *   返回: 函数地址, 找不到返回 NULL */
void *wine_builtin_lookup(const char *dll, const char *name);

/* wine_set_locale: 切换当前进程代码页 (Phase 2 完整实现, Phase 1 仅记录) */
void wine_set_locale(int codepage);

/* ============================================================
 * 函数指针表 (解决 64 位宿主 vs 32 位 PE IAT 的不匹配)
 * ============================================================
 *
 * 问题: PE32 的 IAT 槽是 32 位, 但 iOS arm64 / macOS x86_64 宿主的函数指针
 *       是 64 位, 无法直接存入 IAT 槽。
 *
 * 方案: 维护全局函数指针表, IAT 槽存 32 位 id (1-based, 0=未解析),
 *       解释器读 id 后用 wine_func_get 查 64 位地址再调用。
 *       id 在进程内稳定, 多次注册同一指针返回同一 id (去重)。 */

/* wine_func_register: 注册一个函数指针, 返回其 id (1-based, 0 表示表满)
 *   已注册的指针返回原 id (去重, 节省空间) */
uint32_t wine_func_register(void *ptr);

/* wine_func_get: 按 id 查函数指针
 *   返回: 函数地址, id=0 或越界返回 NULL */
void *wine_func_get(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* WINE_WINE_H */
