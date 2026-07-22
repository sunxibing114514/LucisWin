/* end_to_end_tests.c — 端到端测试: 跑通 hello.exe 完整调用链 (RED 阶段)
 *
 * 验证: PE 加载 → 解释器执行 → MessageBoxA 钩子拦截 → Shift-JIS 解码 → 正确文本
 *
 * 流程:
 *   1. 安装测试用 messagebox 钩子, 捕获 text/caption/flags 到全局变量
 *   2. 调用 wine_run_exe(hello.exe 字节)
 *   3. 断言钩子被调用恰好 1 次
 *   4. 断言捕获的 UTF-16 text 解码后等于 "こんにちは、iOSのWineです！"
 *   5. 断言 wine_run_exe 返回 0 (ExitProcess(0))
 *
 * 期望文本 (Shift-JIS 解码后的 UTF-16LE):
 *   こ(3053) ん(3093) に(306B) ち(3061) は(306F) 、(3001)
 *   i(0069) O(004F) S(0053) の(306E) W(0057) i(0069) n(006E) e(0065)
 *   で(3067) す(3059) ！(FF01)
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

/* ---- hello.exe 加载 (同 pe_loader_tests.c) ---- */
static uint8_t *load_hello_exe(size_t *out_len) {
    const char *env = getenv("LUCISWIN_HELLO_EXE");
    char path[1024];
    if (env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 16);
        if (n < 0) { *out_len = 0; return NULL; }
        path[n] = 0;
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0;
        strcat(path, "hello.exe");
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

/* 期望的 UTF-16LE 文本: こんにちは、iOSのWineです！\0 */
static const uint16_t c_expected_text[] = {
    0x3053, 0x3093, 0x306B, 0x3061, 0x306F, 0x3001,
    0x0069, 0x004F, 0x0053, 0x306E,
    0x0057, 0x0069, 0x006E, 0x0065,
    0x3067, 0x3059, 0xFF01, 0x0000
};

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

/* 端到端: hello.exe 调用 MessageBoxA 后 ExitProcess(0) */
TEST(e2e_hello_exe_messagebox) {
    size_t len; uint8_t *data = load_hello_exe(&len);
    ASSERT(data != NULL, "无法加载 hello.exe");

    /* 安装捕获钩子, 清零状态 */
    user32_set_messagebox_hook(test_hook);
    g_hook_called = 0;
    g_text[0] = 0;
    g_caption[0] = 0;

    int ret = wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called == 1,
           "钩子应被调用 1 次, 实际 %d", g_hook_called);
    ASSERT(ret == 0, "wine_run_exe 返回 %d (期望 0)", ret);
    ASSERT(u16_eq(g_text, c_expected_text),
           "文本不匹配 (Shift-JIS 解码错误?)");
}

/* 端到端: caption 应为 "luciswin" */
TEST(e2e_hello_exe_caption) {
    size_t len; uint8_t *data = load_hello_exe(&len);
    ASSERT(data != NULL, "无法加载 hello.exe");

    user32_set_messagebox_hook(test_hook);
    g_hook_called = 0;
    g_caption[0] = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called == 1, "钩子未被调用");
    /* caption = "luciswin" in UTF-16LE */
    static const uint16_t cap[] = {
        'l','u','c','i','s','w','i','n', 0
    };
    ASSERT(u16_eq(g_caption, cap), "caption 不匹配");
}

/* 端到端: flags 应为 MB_OK = 0 */
TEST(e2e_hello_exe_flags) {
    size_t len; uint8_t *data = load_hello_exe(&len);
    ASSERT(data != NULL, "无法加载 hello.exe");

    user32_set_messagebox_hook(test_hook);
    g_hook_called = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called == 1, "钩子未被调用");
    ASSERT(g_flags == 0, "flags=%u (期望 MB_OK=0)", g_flags);
}
