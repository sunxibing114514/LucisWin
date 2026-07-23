/* fib_end_to_end_tests.c — 端到端测试: 跑通 fib.exe 完整调用链 (Phase 2.1)
 *
 * 验证: PE 加载 → 解释器执行扩展指令集 (DIV/MOVZX/字节 mov/近 Jcc) →
 *       MessageBoxA 钩子拦截 → ASCII 文本正确 → ExitProcess(0)
 *
 * fib.exe 计算 fib(20)=6765, 自写 itoa 拼成 "fib(20)=6765" 后 MessageBoxA 显示。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/wine.h"
#include "wine/pe_loader.h"
#include "../WineCore/dlls/user32/messagebox_hook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- fib.exe 加载 ---- */
static uint8_t *load_fib_exe(size_t *out_len) {
    const char *env = getenv("LUCISWIN_FIB_EXE");
    char path[1024];
    if (env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 16);
        if (n < 0) { *out_len = 0; return NULL; }
        path[n] = 0;
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0;
        strcat(path, "fib.exe");
    }
    FILE *f = fopen(path, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    fread(buf, 1, len, f);
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

/* ---- 钩子捕获状态 ---- */
static int      g_hook_called = 0;
static uint16_t g_text[256];
static uint16_t g_caption[64];
static uint32_t g_flags;
static int      g_result;

static void test_hook(const uint16_t *text, const uint16_t *caption,
                      uint32_t flags, int *result) {
    g_hook_called++;
    g_flags = flags;
    *result = 1; /* IDOK */
    if (text) {
        int i;
        for (i = 0; i < 255 && text[i]; i++) g_text[i] = text[i];
        g_text[i] = 0;
    } else { g_text[0] = 0; }
    if (caption) {
        int i;
        for (i = 0; i < 63 && caption[i]; i++) g_caption[i] = caption[i];
        g_caption[i] = 0;
    } else { g_caption[0] = 0; }
}

/* 比较两个 UTF-16 字符串 (含终止符) */
static int u16_eq(const uint16_t *a, const uint16_t *b) {
    size_t i = 0;
    while (a[i] == b[i]) {
        if (a[i] == 0) return 1;
        i++;
    }
    return 0;
}

/* ---- 测试用例 ---- */

/* fib.exe 应输出 "fib(20)=6765" */
TEST(fib_e2e_messagebox) {
    size_t len; uint8_t *data = load_fib_exe(&len);
    ASSERT(data != NULL, "无法加载 fib.exe");

    user32_set_messagebox_hook(test_hook);
    g_hook_called = 0;
    g_text[0] = 0;

    int ret = wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called == 1,
           "钩子应被调用 1 次, 实际 %d", g_hook_called);
    ASSERT(ret == 0, "wine_run_exe 返回 %d (期望 0)", ret);
    /* "fib(20)=6765" */
    static const uint16_t expected[] = {
        'f','i','b','(', '2','0',')','=', '6','7','6','5', 0
    };
    ASSERT(u16_eq(g_text, expected),
           "文本不匹配 (实际: %s)", g_text);
}

/* caption 应为 "luciswin" */
TEST(fib_e2e_caption) {
    size_t len; uint8_t *data = load_fib_exe(&len);
    ASSERT(data != NULL, "无法加载 fib.exe");

    user32_set_messagebox_hook(test_hook);
    g_hook_called = 0;
    g_caption[0] = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called == 1, "钩子未被调用");
    static const uint16_t cap[] = {
        'l','u','c','i','s','w','i','n', 0
    };
    ASSERT(u16_eq(g_caption, cap), "caption 不匹配");
}

/* flags 应为 MB_OK = 0 */
TEST(fib_e2e_flags) {
    size_t len; uint8_t *data = load_fib_exe(&len);
    ASSERT(data != NULL, "无法加载 fib.exe");

    user32_set_messagebox_hook(test_hook);
    g_hook_called = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called == 1, "钩子未被调用");
    ASSERT(g_flags == 0, "flags=%u (期望 MB_OK=0)", g_flags);
}
