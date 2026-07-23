/* notepad_end_to_end_tests.c — Phase 3.4 端到端测试: notepad.exe 完整流程
 *
 * 验证 comctl32 + Edit 控件全链路:
 *   PE 加载 → 解释器执行 → comctl32 导入解析 →
 *   InitCommonControls → RegisterClassExW → CreateWindowExW(父) →
 *   CreateWindowExW(Edit 子) → SetWindowTextW(触发 EN_CHANGE) →
 *   WM_COMMAND 派发 → BeginPaint 发 PAINT_EDIT → WM_DESTROY → ExitProcess(0)
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

/* ---- notepad.exe 加载 ---- */
static uint8_t *load_notepad_exe(size_t *out_len) {
    const char *env = getenv("LUCISWIN_NOTEPAD_EXE");
    char path[1024];
    if (env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 16);
        if (n < 0) { *out_len = 0; return NULL; }
        path[n] = 0;
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0;
        strcat(path, "notepad.exe");
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

/* 在捕获的绘制命令里找首个 PAINT_EDIT, 返回索引; 找不到返回 -1 */
static int find_paint_edit(void) {
    for (int i = 0; i < g_cmd_count; i++) {
        if (g_cmds[i].kind == PAINT_EDIT) return i;
    }
    return -1;
}

/* ---- 测试用例 ---- */

/* 退出码应为 0 (WM_DESTROY → PostQuitMessage → 循环退出 → ExitProcess) */
TEST(notepad_e2e_exit_code) {
    size_t len; uint8_t *data = load_notepad_exe(&len);
    ASSERT(data != NULL, "无法加载 notepad.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    int ret = wine_run_exe(data, len);
    free(data);

    ASSERT(ret == 0, "notepad.exe 退出码 %d (期望 0)", ret);
}

/* 绘制钩子应被调用, 且至少含一条 PAINT_EDIT 命令 (Edit 子窗口代绘) */
TEST(notepad_e2e_paint_edit_captured) {
    size_t len; uint8_t *data = load_notepad_exe(&len);
    ASSERT(data != NULL, "无法加载 notepad.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    ASSERT(g_hook_called >= 1, "绘制钩子应至少调用 1 次, 实际 %d", g_hook_called);
    int idx = find_paint_edit();
    ASSERT(idx >= 0,
           "应捕获到 PAINT_EDIT 命令 (cmd_count=%d)", g_cmd_count);
}

/* PAINT_EDIT 文本应为 "hello notepad" (SetWindowText 设置的初始文本) */
TEST(notepad_e2e_edit_text) {
    size_t len; uint8_t *data = load_notepad_exe(&len);
    ASSERT(data != NULL, "无法加载 notepad.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    int idx = find_paint_edit();
    ASSERT(idx >= 0, "未捕获到 PAINT_EDIT 命令");
    static const uint16_t expected[] = {
        'h','e','l','l','o',' ','n','o','t','e','p','a','d', 0
    };
    ASSERT(u16_eq(g_cmds[idx].text, expected),
           "PAINT_EDIT 文本应为 \"hello notepad\"");
}

/* PAINT_EDIT 几何: x=10, y=10, x2=390, y2=190 (Edit 子窗口位置 10,10,380,180) */
TEST(notepad_e2e_edit_geometry) {
    size_t len; uint8_t *data = load_notepad_exe(&len);
    ASSERT(data != NULL, "无法加载 notepad.exe");

    wine_set_paint_hook(paint_hook);
    g_hook_called = 0;
    g_cmd_count = 0;

    wine_run_exe(data, len);
    free(data);

    int idx = find_paint_edit();
    ASSERT(idx >= 0, "未捕获到 PAINT_EDIT 命令");
    ASSERT(g_cmds[idx].x == 10 && g_cmds[idx].y == 10,
           "Edit 左上应为 (10,10), 实际 (%d,%d)",
           g_cmds[idx].x, g_cmds[idx].y);
    ASSERT(g_cmds[idx].x2 == 390 && g_cmds[idx].y2 == 190,
           "Edit 右下应为 (390,190), 实际 (%d,%d)",
           g_cmds[idx].x2, g_cmds[idx].y2);
}
