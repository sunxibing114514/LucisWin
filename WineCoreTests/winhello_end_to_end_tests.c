/* winhello_end_to_end_tests.c — 端到端测试: 跑通 winhello.exe 完整窗口路径
 *
 * Phase 2.2: 验证 RegisterClassExW → CreateWindowExW → 消息循环 →
 *            WndProc (BeginPaint/TextOutW/EndPaint) → WM_DESTROY → ExitProcess
 * Phase 3.1: 扩展 WndProc 用 GDI 对象模型绘制 rect+line+text,
 *            断言 paint_hook 收到 3 条命令 (RECT/LINE/TEXT) 与颜色/坐标
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

static int u16_eq(const uint16_t *a, const uint16_t *b) {
    size_t i = 0;
    while (a[i] == b[i]) {
        if (a[i] == 0) return 1;
        i++;
    }
    return 0;
}

/* ---- 测试用例 ---- */

/* Phase 3.1: 应收到 3 条命令 (RECT/LINE/TEXT), 第三条文本含 "luciswin gdi32" */
TEST(winhello_e2e_paint_text) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    int ret = wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called >= 1, "绘制钩子应至少调用 1 次, 实际 %d", g_hook_called);
    ASSERT(ret == 0, "wine_run_exe 返回 %d (期望 0)", ret);
    ASSERT(g_cmd_count >= 3, "应有至少 3 条绘制命令 (rect+line+text), 实际 %d", g_cmd_count);
    /* 第三条应是 TEXT, 文本含 "luciswin gdi32" */
    ASSERT(g_cmds[2].kind == PAINT_TEXT, "第三条应为 PAINT_TEXT, 实际 %d", g_cmds[2].kind);
    static const uint16_t expected[] = {
        'l','u','c','i','s','w','i','n',' ','g','d','i','3','2', 0
    };
    ASSERT(u16_eq(g_cmds[2].text, expected),
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

/* Phase 3.1: 首条命令应是 RECTANGLE, 坐标 (10,10,100,60) */
TEST(winhello_e2e_paint_coords) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_cmd_count >= 1, "应有绘制命令");
    ASSERT(g_cmds[0].kind == PAINT_RECTANGLE, "首条应为 RECTANGLE, 实际 %d", g_cmds[0].kind);
    ASSERT(g_cmds[0].x == 10 && g_cmds[0].y == 10,
           "矩形左上应为 (10,10), 实际 (%d,%d)", g_cmds[0].x, g_cmds[0].y);
    ASSERT(g_cmds[0].x2 == 100 && g_cmds[0].y2 == 60,
           "矩形右下应为 (100,60), 实际 (%d,%d)", g_cmds[0].x2, g_cmds[0].y2);
}

/* Phase 3.1: 矩形边框色=红 (0x000000FF), 填充色=蓝 (0x00FF0000) */
TEST(winhello_e2e_rect_colors) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_cmd_count >= 1, "应有绘制命令");
    ASSERT(g_cmds[0].kind == PAINT_RECTANGLE, "首条应为 RECTANGLE");
    ASSERT(g_cmds[0].color == 0x000000FF,
           "边框色应为红 0xFF, 实际 %#x", g_cmds[0].color);
    ASSERT(g_cmds[0].fill_color == 0x00FF0000,
           "填充色应为蓝 0xFF0000, 实际 %#x", g_cmds[0].fill_color);
}

/* Phase 3.1: 第二条应是 LINE, 从 (10,70) 到 (200,70) */
TEST(winhello_e2e_line) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_cmd_count >= 2, "应有 >=2 条命令, 实际 %d", g_cmd_count);
    ASSERT(g_cmds[1].kind == PAINT_LINE, "第二条应为 LINE, 实际 %d", g_cmds[1].kind);
    ASSERT(g_cmds[1].x == 10 && g_cmds[1].y == 70,
           "线起点应为 (10,70), 实际 (%d,%d)", g_cmds[1].x, g_cmds[1].y);
    ASSERT(g_cmds[1].x2 == 200 && g_cmds[1].y2 == 70,
           "线终点应为 (200,70), 实际 (%d,%d)", g_cmds[1].x2, g_cmds[1].y2);
}

/* Phase 3.1: 第三条文本颜色应为绿 (0x00008000) */
TEST(winhello_e2e_text_color) {
    size_t len; uint8_t *data = load_winhello_exe(&len);
    ASSERT(data != NULL, "无法加载 winhello.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_cmd_count >= 3, "应有 >=3 条命令, 实际 %d", g_cmd_count);
    ASSERT(g_cmds[2].kind == PAINT_TEXT, "第三条应为 TEXT");
    ASSERT(g_cmds[2].color == 0x00008000,
           "文本色应为绿 0x8000, 实际 %#x", g_cmds[2].color);
    ASSERT(g_cmds[2].x == 10 && g_cmds[2].y == 80,
           "文本坐标应为 (10,80), 实际 (%d,%d)", g_cmds[2].x, g_cmds[2].y);
}
