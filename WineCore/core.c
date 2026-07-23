/* core.c — luciswin 核心入口
 *
 * 功能:
 *   1. 维护内建 DLL 导出注册表 (wine_builtin_register/lookup)
 *   2. wine_init: 注册 Phase 1 内建 DLL (kernel32/user32/msvcrt) 的导出
 *   3. wine_run_exe: 加载 PE + 启动解释器, 直到 ExitProcess (Phase 1 框架)
 *
 * 设计说明:
 *   - 内建注册表用简单线性表 (Phase 1 容量足够), Phase 2 可换哈希表加速查找
 *   - stub 函数 (wine_stub_*) 是占位实现, 后续 TDD 循环在各 DLL 文件实现真实版本后,
 *     core.c 的 wine_init 改为引用真实符号; 当前 stub 让 PE 导入解析能填充 IAT
 *   - wine_run_exe 的解释器调用在 interpreter_tests/end_to_end TDD 循环完成
 */
#include "wine/wine.h"
#include "wine/pe_loader.h"
#include "wine/cpu.h"
#include "wine/paint_hook.h"  /* wine_window_reset */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp (POSIX) */

/* ============================================================
 * 函数指针表 (见 wine.h 说明)
 * ============================================================ */

#define WINE_FUNC_TABLE_MAX 4096
static void *g_func_table[WINE_FUNC_TABLE_MAX];
static int g_func_table_count = 0;

/* wine_func_register: 注册函数指针, 返回 id (>= WINE_FUNC_ID_BASE, 0=表满)
 *   去重: 已注册的 ptr 返回原 id (线性查重, Phase 1 表小可接受) */
uint32_t wine_func_register(void *ptr) {
    if (!ptr) return 0;
    /* 去重查找 */
    for (int i = 0; i < g_func_table_count; i++) {
        if (g_func_table[i] == ptr) return WINE_FUNC_ID_BASE + (uint32_t)i;
    }
    if (g_func_table_count >= WINE_FUNC_TABLE_MAX) return 0;
    g_func_table[g_func_table_count] = ptr;
    return WINE_FUNC_ID_BASE + (uint32_t)(g_func_table_count++);
}

/* wine_func_get: 按 id 查函数指针 (id < WINE_FUNC_ID_BASE 或越界返回 NULL) */
void *wine_func_get(uint32_t id) {
    if (id < WINE_FUNC_ID_BASE) return NULL;
    uint32_t idx = id - WINE_FUNC_ID_BASE;
    if (idx >= (uint32_t)g_func_table_count) return NULL;
    return g_func_table[idx];
}

/* ============================================================
 * 内建 DLL 导出注册表
 * ============================================================ */

#define WINE_BUILTIN_DLL_MAX 16

static wine_builtin_dll_t g_builtins[WINE_BUILTIN_DLL_MAX];
static int g_builtin_count = 0;
static int g_initialized = 0;

/* wine_builtin_register: 注册一个内建 DLL 的导出表
 *   重复注册同名 DLL 会追加其导出 (支持分文件注册同一 DLL 的不同函数) */
void wine_builtin_register(const char *dll, wine_export_t *exports) {
    if (g_builtin_count >= WINE_BUILTIN_DLL_MAX) return;
    g_builtins[g_builtin_count].dll = dll;
    g_builtins[g_builtin_count].exports = exports;
    g_builtin_count++;
}

/* wine_builtin_lookup: 按函数名查找内建导出地址
 *   dll/name 大小写不敏感比较 DLL 名, 函数名大小写敏感 (Win32 导出名区分大小写)
 *   性能: 线性扫描, Phase 1 内建 DLL 数量少 (<10) 可接受 */
void *wine_builtin_lookup(const char *dll, const char *name) {
    if (!dll || !name) return NULL;
    for (int i = 0; i < g_builtin_count; i++) {
        /* strcasecmp 在 POSIX, 大小写不敏感比较 DLL 名 */
        if (strcasecmp(g_builtins[i].dll, dll) == 0) {
            for (wine_export_t *e = g_builtins[i].exports; e->name; e++) {
                if (strcmp(e->name, name) == 0) return e->addr;
            }
        }
    }
    return NULL;
}

/* Phase 1 内建 DLL thunk 声明 (在各 .c 文件实现)
 * ============================================================ */
/* user32_MessageBoxA_thunk: 读栈参数, 转码, 调用钩子 */
extern void user32_MessageBoxA_thunk(cpu_context_t *ctx);
/* kernel32_ExitProcess_thunk: 读退出码, longjmp 回 cpu_run */
extern void kernel32_ExitProcess_thunk(cpu_context_t *ctx);
/* msvcrt_atexit_thunk: 空实现 (Phase 1 不支持 atexit 回调) */
extern void msvcrt_atexit_thunk(cpu_context_t *ctx);

/* Phase 2.2 窗口 API thunks (user32, 在 window.c 实现) */
extern void user32_RegisterClassExW_thunk(cpu_context_t *ctx);
extern void user32_CreateWindowExW_thunk(cpu_context_t *ctx);
extern void user32_ShowWindow_thunk(cpu_context_t *ctx);
extern void user32_UpdateWindow_thunk(cpu_context_t *ctx);
extern void user32_GetMessageW_thunk(cpu_context_t *ctx);
extern void user32_TranslateMessage_thunk(cpu_context_t *ctx);
extern void user32_DispatchMessageW_thunk(cpu_context_t *ctx);
extern void user32_PostQuitMessage_thunk(cpu_context_t *ctx);
extern void user32_DefWindowProcW_thunk(cpu_context_t *ctx);
extern void user32_BeginPaint_thunk(cpu_context_t *ctx);
extern void user32_EndPaint_thunk(cpu_context_t *ctx);
extern void user32_GetClientRect_thunk(cpu_context_t *ctx);
/* GDI thunks (gdi32, 在 window.c 同文件实现) */
extern void gdi32_TextOutW_thunk(cpu_context_t *ctx);

/* Phase 1 内建导出表 (thunk 函数指针) */
static wine_export_t g_kernel32_exports[] = {
    {"ExitProcess", (void*)kernel32_ExitProcess_thunk},
    {NULL, NULL}
};
static wine_export_t g_user32_exports[] = {
    {"MessageBoxA",     (void*)user32_MessageBoxA_thunk},
    /* Phase 2.2 窗口 API */
    {"RegisterClassExW",(void*)user32_RegisterClassExW_thunk},
    {"CreateWindowExW", (void*)user32_CreateWindowExW_thunk},
    {"ShowWindow",      (void*)user32_ShowWindow_thunk},
    {"UpdateWindow",    (void*)user32_UpdateWindow_thunk},
    {"GetMessageW",     (void*)user32_GetMessageW_thunk},
    {"TranslateMessage",(void*)user32_TranslateMessage_thunk},
    {"DispatchMessageW",(void*)user32_DispatchMessageW_thunk},
    {"PostQuitMessage", (void*)user32_PostQuitMessage_thunk},
    {"DefWindowProcW",  (void*)user32_DefWindowProcW_thunk},
    {"BeginPaint",      (void*)user32_BeginPaint_thunk},
    {"EndPaint",        (void*)user32_EndPaint_thunk},
    {"GetClientRect",   (void*)user32_GetClientRect_thunk},
    {NULL, NULL}
};
static wine_export_t g_msvcrt_exports[] = {
    {"atexit", (void*)msvcrt_atexit_thunk},
    {NULL, NULL}
};
static wine_export_t g_gdi32_exports[] = {
    {"TextOutW", (void*)gdi32_TextOutW_thunk},
    {NULL, NULL}
};

/* wine_set_locale: 切换代码页 (Phase 1 仅记录, Phase 2 完整实现) */
void wine_set_locale(int codepage) {
    fprintf(stderr, "[wine] set_locale(%d) — Phase 2 实现\n", codepage);
}

/* wine_init: 初始化运行时, 注册内建 DLL
 *   幂等: 重复调用安全 */
int wine_init(void) {
    if (g_initialized) return 0;
    wine_builtin_register("kernel32.dll", g_kernel32_exports);
    wine_builtin_register("user32.dll",   g_user32_exports);
    wine_builtin_register("msvcrt.dll",    g_msvcrt_exports);
    wine_builtin_register("gdi32.dll",    g_gdi32_exports);
    g_initialized = 1;
    return 0;
}

/* wine_run_exe: 加载 PE, 设置 CPU 上下文与栈, 跑解释器直到 ExitProcess
 *   返回: ExitProcess 的退出码, 负数表示加载失败 */
int wine_run_exe(const uint8_t *data, size_t len) {
    wine_init();
    pe_image_t img = {0};
    pe_error_t err = pe_load_image(data, len, "exe", &img);
    if (err != PE_OK) {
        fprintf(stderr, "[wine] PE 加载失败: %s\n", pe_last_error_string(err));
        return -1;
    }

    /* Phase 2.2: 每次运行前重置窗口管理器状态 (类注册表/窗口表/消息队列) */
    wine_window_reset();

    /* 扩展镜像内存: 在 PE 镜像后附加栈空间
     *   guest 内存布局: [0, img.size) = PE 镜像, [img.size, img.size+STACK) = 栈
     *   栈向下生长, ESP 初始 = img.size + STACK - 64 */
    #define WINE_STACK_SIZE (256 * 1024)
    uint8_t *mem = (uint8_t *)realloc(img.base, img.size + WINE_STACK_SIZE);
    if (!mem) {
        pe_free_image(&img);
        return -1;
    }
    memset(mem + img.size, 0, WINE_STACK_SIZE);
    img.base = mem;

    /* 设置 CPU 上下文 */
    cpu_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mem_base = mem;
    ctx.mem_size = img.size + WINE_STACK_SIZE;
    ctx.current_image = &img;
    ctx.gpr[CPU_REG_ESP] = img.size + WINE_STACK_SIZE - 64;
    /* 压入伪造返回地址 0 (main 若 ret 会跳到 offset 0, 可能触发 #UD — 但 hello.exe 调 ExitProcess 不 ret) */
    ctx.gpr[CPU_REG_ESP] -= 4;
    cpu_mem_w32(&ctx, ctx.gpr[CPU_REG_ESP], 0);

    /* 执行 */
    cpu_status_t st = cpu_run(&ctx, img.entry_point);

    int exit_code = (int)ctx.exit_code;
    free(img.base);
    img.base = NULL;

    if (st == CPU_ERROR_UD) {
        fprintf(stderr, "[wine] #UD at EIP=%#x, exit_code=%#x\n",
                ctx.eip, ctx.exit_code);
        return -1;
    }
    return exit_code;
}
