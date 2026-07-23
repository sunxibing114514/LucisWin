/* winhello_end_to_end_tests.c — 端到端测试: 跑通 winhello.exe 完整窗口路径 (Phase 2.2)
 *
 * 验证: PE 加载 → RegisterClassExW → CreateWindowExW → 消息循环 →
 *       WndProc (BeginPaint/TextOutW/EndPaint) → WM_DESTROY/PostQuitMessage →
 *       ExitProcess(0)
 *
 * winhello.exe 在 WM_PAINT 用 TextOutW 绘制 "luciswin Win32 window"。
 * 测试安装绘制钩子捕获命令, 验证文本与退出码。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/wine.h"
#include "wine/pe_loader.h"
#include "../WineCore/include/wine/paint_hook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- winhello.exe 加载 ---- */
static uint8_t *load_winhello_exe(size_t *out_len) {
    const char *env = getenv("LUCISWIN_WINHELLO_EXE");
    char path[1024];
    if (env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 16);
        if (n < 0) { *out_len = 0; return NULL; }
        path[n] = 0;
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0;
        strcat(path, "winhello.exe");
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

/* ---- 绘制钩子捕获状态 ---- */
static int             g_hook_called = 0;
static wine_paint_cmd_t g_cmds[16];
static int             g_cmd_count = 0;

static void paint_hook(const wine_paint_cmd_t *cmds, int count) {
    g_hook_called++;
    g_cmd_count = count;
    int n = count;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) g_cmds[i] = cmds[i];
}

/* 比较两个 UTF-16 字符串 */
static int u16_eq(const uint16_t *a, const uint16_t *b) {
    size_t i = 0;
    while (a[i] == b[i]) {
        if (a[i] == 0) return 1;
        i++;
    }
    return 0;
}

/* ---- 测试用例 ---- */

/* winhello.exe 应通过完整窗口路径, TextOutW 收到 "luciswin Win32 window" */
TEST(winhello_e2e_paint_text) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    int ret = wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called >= 1,
           "绘制钩子应至少调用 1 次, 实际 %d", g_hook_called);
    ASSERT(ret == 0, "wine_run_exe 返回 %d (期望 0)", ret);
    /* 首条命令文本应为 "luciswin Win32 window" */
    ASSERT(g_cmd_count >= 1, "应有至少 1 条绘制命令, 实际 %d", g_cmd_count);
    static const uint16_t expected[] = {
        'l','u','c','i','s','w','i','n',' ',
        'W','i','n','3','2',' ','w','i','n','d','o','w', 0
    };
    ASSERT(u16_eq(g_cmds[0].text, expected),
           "TextOutW 文本不匹配 (cmd_count=%d)", g_cmd_count);
}

/* ExitProcess 应正常返回 0 (WM_DESTROY→PostQuitMessage→循环退出→ExitProcess) */
TEST(winhello_e2e_exit_code) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;

    int ret = wine_run_exe(data, len);
    free(data);

    ASSERT(ret == 0, "winhello.exe 退出码 %d (期望 0)", ret);
}

/* 绘制命令坐标应为 (10, 10) (winhello.c 中 TextOutW(hdc, 10, 10, ...)) */
TEST(winhello_e2e_paint_coords) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_cmd_count >= 1, "应有绘制命令");
    ASSERT(g_cmds[0].x == 10 && g_cmds[0].y == 10,
           "坐标应为 (10,10), 实际 (%d,%d)", g_cmds[0].x, g_cmds[0].y);
}
