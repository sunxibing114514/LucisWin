/* window.c — Win32 窗口管理器实现 (Phase 2.2)
 *
 * 维护窗口类注册表、窗口对象表 (HWND→对象)、消息队列、绘制命令缓冲。
 * 宿主无关: 不依赖 UIKit, 通过 paint_hook 回调把绘制命令交给宿主
 * (测试用捕获钩子, iOS 用 UIView/CGContext 渲染)。
 *
 * 关键设计:
 *   - WndProc 是 guest 函数地址, DispatchMessage 时解释器调用 guest 代码
 *   - 消息队列按窗口分桶, GetMessage 取队首; 空队列 + WM_QUIT 标志 → 返回 0
 *   - 绘制命令: TextOutW 把命令存到当前 paint 的命令列表,
 *     EndPaint 调钩子回放 (测试断言 / iOS 渲染)
 */
#include "wine/cpu.h"
#include "wine/win32_types.h"
#include "wine/paint_hook.h"
#include "wine/gdi.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- 窗口类注册表 ---- */
#define WINE_CLASS_MAX 16
typedef struct {
    char     name[64];     /* 类名 (窄字符比较, 注册时按字节) */
    uint32_t wnd_proc;     /* guest WndProc 地址 */
    int      used;
} wine_class_t;
static wine_class_t g_classes[WINE_CLASS_MAX];

/* ---- 窗口对象表 ---- */
#define WINE_WINDOW_MAX 32
typedef struct {
    uint32_t hwnd;          /* 1-based id */
    int      class_idx;     /* 指向 g_classes */
    uint32_t quitted;       /* PostQuitMessage 设置 */
    /* 消息队列 */
    win32_msg_t queue[64];
    int         q_head, q_tail;
    int          used;
} wine_window_t;
static wine_window_t g_windows[WINE_WINDOW_MAX];

/* ---- 当前绘制上下文 ----
 * BeginPaint 设置, TextOutW 写入, EndPaint 调钩子后清空。
 * g_paint_cmds/cmd_count/hdc 由 gdi.c (TextOutW/Rectangle/LineTo) extern 引用,
 * 故不能 static — 改为全局链接。 */
wine_window_t *g_paint_window = NULL;
uint32_t g_paint_hdc = 0;

/* 绘制命令缓冲 — 用 wine_paint_cmd_t 与 paint_hook.h 共享类型 */
#define WINE_PAINT_CMD_MAX 16
wine_paint_cmd_t g_paint_cmds[WINE_PAINT_CMD_MAX];
int g_paint_cmd_count = 0;

/* 绘制钩子 (EndPaint 时调用, 传命令数组给宿主) */
static wine_paint_hook_t g_paint_hook = NULL;
void wine_set_paint_hook(wine_paint_hook_t hook) { g_paint_hook = hook; }

/* ---- 全局 WM_QUIT 标志 (PostQuitMessage 设置) ---- */
static int g_quit_code = -1;  /* -1 = 未退出 */

/* ---- 辅助: 查窗口 ---- */
static wine_window_t *hwnd_to_win(uint32_t hwnd) {
    if (hwnd == 0 || hwnd > WINE_WINDOW_MAX) return NULL;
    wine_window_t *w = &g_windows[hwnd - 1];
    return w->used ? w : NULL;
}

/* ---- 窗口类注册 (RegisterClassExW thunk) ----
 * guest 传 WNDCLASSEXW 指针, 我们读 lpszClassName 与 lpfnWndProc。
 * 类名是 UTF-16, 但 Phase 2.2 用窄字节比较 (类名均为 ASCII)。
 * 返回 1=成功 (ATOM), 0=失败 */

uint32_t user32_RegisterClassExW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t lpwcx = cpu_mem_r32(ctx, esp + 4);
    if (!lpwcx || !ctx->current_image) { ctx->gpr[CPU_REG_EAX] = 0; return 0; }
    uint8_t *p = (uint8_t *)ctx->mem_base + lpwcx;
    win32_wndclassex_t *wc = (win32_wndclassex_t *)p;
    uint32_t name_ga = wc->lpszClassName;
    const uint16_t *wname = (const uint16_t *)((uint8_t *)ctx->mem_base + name_ga);

    /* 找空槽 */
    int slot = -1;
    for (int i = 0; i < WINE_CLASS_MAX; i++) {
        if (!g_classes[i].used) { slot = i; break; }
    }
    if (slot < 0) { ctx->gpr[CPU_REG_EAX] = 0; return 0; }

    /* 类名存为窄字节 (UTF-16 低字节, ASCII 兼容) */
    int n = 0;
    for (; n < 63 && wname[n]; n++) g_classes[slot].name[n] = (char)(wname[n] & 0xFF);
    g_classes[slot].name[n] = 0;
    g_classes[slot].wnd_proc = wc->lpfnWndProc;
    g_classes[slot].used = 1;

    /* 返回伪 ATOM (非 0) */
    ctx->gpr[CPU_REG_EAX] = (uint32_t)(slot + 1);
    return (uint32_t)(slot + 1);
}

/* ---- 查类名找 class_idx ---- */
static int find_class(const uint16_t *wname) {
    for (int i = 0; i < WINE_CLASS_MAX; i++) {
        if (!g_classes[i].used) continue;
        int j = 0;
        for (; wname[j] && j < 63; j++) {
            if ((char)(wname[j] & 0xFF) != g_classes[i].name[j]) break;
        }
        if (wname[j] == 0 && g_classes[i].name[j] == 0) return i;
    }
    return -1;
}

/* ---- CreateWindowExW thunk ----
 * 参数: dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, w, h,
 *       hWndParent, hMenu, hInstance, lpParam
 * 返回 HWND (1-based id) */
uint32_t user32_CreateWindowExW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t class_ga = cpu_mem_r32(ctx, esp + 8);
    const uint16_t *wname = (const uint16_t *)((uint8_t *)ctx->mem_base + class_ga);
    int class_idx = find_class(wname);
    if (class_idx < 0) { ctx->gpr[CPU_REG_EAX] = 0; return 0; }

    /* 找空窗口槽 */
    int slot = -1;
    for (int i = 0; i < WINE_WINDOW_MAX; i++) {
        if (!g_windows[i].used) { slot = i; break; }
    }
    if (slot < 0) { ctx->gpr[CPU_REG_EAX] = 0; return 0; }

    wine_window_t *w = &g_windows[slot];
    memset(w, 0, sizeof(*w));
    w->hwnd = (uint32_t)(slot + 1);
    w->class_idx = class_idx;
    w->used = 1;
    w->quitted = 0;
    w->q_head = w->q_tail = 0;

    /* 触发初始 WM_CREATE → 但 Phase 2.2 跳过, 直接给一个 WM_PAINT */
    /* 投递一个 WM_PAINT 让消息循环能立刻绘制 */
    win32_msg_t m = {0};
    m.hwnd = w->hwnd;
    m.message = WM_PAINT;
    w->queue[w->q_tail] = m;
    w->q_tail = (w->q_tail + 1) % 64;

    ctx->gpr[CPU_REG_EAX] = w->hwnd;
    return w->hwnd;
}

/* ---- ShowWindow / UpdateWindow: 空实现 (Phase 2.2) ---- */
void user32_ShowWindow_thunk(cpu_context_t *ctx) {
    /* 返回 TRUE (之前可见), Phase 2.2 总是返回 1 */
    ctx->gpr[CPU_REG_EAX] = 1;
}

void user32_UpdateWindow_thunk(cpu_context_t *ctx) {
    /* 空实现: UpdateWindow 会立即触发 WM_PAINT, 但我们已在 CreateWindow 投递 */
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* ---- GetMessageW thunk ----
 * 参数: lpMsg (out), hWnd, wMsgFilterMin, wMsgFilterMax
 * 返回: 0=WM_QUIT, >0=取到消息, -1=错误
 * Phase 2.2: 从窗口队列取, 空队列 + 全局 quit → 取 WM_QUIT 返回 0 */
uint32_t user32_GetMessageW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t msg_ga = cpu_mem_r32(ctx, esp + 4);
    uint32_t hwnd = cpu_mem_r32(ctx, esp + 8);
    (void)hwnd;

    /* 检查全局 quit (PostQuitMessage 设置) */
    if (g_quit_code >= 0) {
        win32_msg_t m = {0};
        m.hwnd = 0;
        m.message = WM_QUIT;
        m.wParam = (uint32_t)g_quit_code;
        /* 写回 lpMsg */
        uint8_t *p = (uint8_t *)ctx->mem_base + msg_ga;
        memcpy(p, &m, sizeof(m));
        ctx->gpr[CPU_REG_EAX] = 0; /* WM_QUIT */
        return 0;
    }

    /* 找一个有消息的窗口 (Phase 2.2: 忽略 hWnd 过滤, 单窗口场景) */
    wine_window_t *w = NULL;
    for (int i = 0; i < WINE_WINDOW_MAX; i++) {
        if (g_windows[i].used && g_windows[i].q_head != g_windows[i].q_tail) {
            w = &g_windows[i];
            break;
        }
    }

    if (w) {
        win32_msg_t m = w->queue[w->q_head];
        w->q_head = (w->q_head + 1) % 64;
        uint8_t *p = (uint8_t *)ctx->mem_base + msg_ga;
        memcpy(p, &m, sizeof(m));
        ctx->gpr[CPU_REG_EAX] = 1; /* 取到消息 */
        return 1;
    }

    /* 无消息且未退出: Phase 2.2 不阻塞, 直接返回 WM_PAINT (重新触发绘制)
     * 避免死循环; 实际应阻塞, 但单窗口无输入场景够用 */
    /* 实际: 返回 WM_QUIT 让循环退出 (防止死循环) */
    win32_msg_t m = {0};
    m.message = WM_QUIT;
    uint8_t *p = (uint8_t *)ctx->mem_base + msg_ga;
    memcpy(p, &m, sizeof(m));
    ctx->gpr[CPU_REG_EAX] = 0;
    return 0;
}

/* ---- TranslateMessage: 空实现 ---- */
void user32_TranslateMessage_thunk(cpu_context_t *ctx) {
    ctx->gpr[CPU_REG_EAX] = 0;
}

/* ---- DispatchMessageW thunk ----
 * 参数: lpMsg (in)
 * 取 msg.hwnd, 查窗口类 WndProc, 模拟 __stdcall call WndProc(hwnd, msg, wp, lp)。
 *
 * 栈布局机制 (配合 interpreter.c FF /2 的 thunk_skip_ret 标志):
 *   主循环 FF /2 调本 thunk 前已 push32(ret_addr), 故 thunk 入口栈:
 *     [ESP]   = ret_addr (主循环 case 后地址)
 *     [ESP+4] = lpMsg 参数
 *   thunk 内部:
 *     1. 读 lpMsg = [ESP+4]
 *     2. pop ret_addr 保存到 R  (ESP += 4)
 *     3. push lParam, wParam, message, hwnd  (stdcall 右到左, 4 个参数)
 *     4. push R  (作为 WndProc 的返回地址)
 *     5. EIP = wndproc; thunk_skip_ret = 1
 *   thunk 返回后主循环见 thunk_skip_ret=1, 跳过 pop32, continue 到 WndProc。
 *   WndProc ret 16 后: pop R (EIP=ret_addr=主循环 case 后), ESP += 16, 栈平衡。
 */
uint32_t user32_DispatchMessageW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t msg_ga = cpu_mem_r32(ctx, esp + 4);
    win32_msg_t *m = (win32_msg_t *)((uint8_t *)ctx->mem_base + msg_ga);

    wine_window_t *w = hwnd_to_win(m->hwnd);
    if (!w || w->class_idx < 0) {
        ctx->gpr[CPU_REG_EAX] = 0;
        return 0;
    }
    uint32_t wndproc = g_classes[w->class_idx].wnd_proc;
    if (!wndproc) { ctx->gpr[CPU_REG_EAX] = 0; return 0; }

    /* 1. pop 主循环压的 ret_addr, 保存到 R */
    uint32_t R = cpu_mem_r32(ctx, ctx->gpr[CPU_REG_ESP]);
    ctx->gpr[CPU_REG_ESP] += 4;

    /* 2. push 4 参数 (stdcall 右到左: lParam, wParam, message, hwnd) */
    ctx->gpr[CPU_REG_ESP] -= 4;
    cpu_mem_w32(ctx, ctx->gpr[CPU_REG_ESP], m->lParam);
    ctx->gpr[CPU_REG_ESP] -= 4;
    cpu_mem_w32(ctx, ctx->gpr[CPU_REG_ESP], m->wParam);
    ctx->gpr[CPU_REG_ESP] -= 4;
    cpu_mem_w32(ctx, ctx->gpr[CPU_REG_ESP], m->message);
    ctx->gpr[CPU_REG_ESP] -= 4;
    cpu_mem_w32(ctx, ctx->gpr[CPU_REG_ESP], m->hwnd);

    /* 3. push R 作为 WndProc 返回地址 (栈顶 = 返回地址) */
    ctx->gpr[CPU_REG_ESP] -= 4;
    cpu_mem_w32(ctx, ctx->gpr[CPU_REG_ESP], R);

    /* 4. 跳 WndProc, 设置 skip 标志让主循环不 pop32 */
    ctx->eip = wndproc;
    ctx->thunk_skip_ret = 1;
    return 0;
}

/* ---- PostQuitMessage thunk ---- */
void user32_PostQuitMessage_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    int32_t code = (int32_t)cpu_mem_r32(ctx, esp + 4);
    g_quit_code = code;
    ctx->gpr[CPU_REG_EAX] = 0;
}

/* ---- DefWindowProcW thunk ----
 * 默认窗口过程: WM_DESTROY → DefWindowProc 不做事 (PostQuitMessage 由用户调),
 * 其余返回 0。Phase 2.2 极简。 */
uint32_t user32_DefWindowProcW_thunk(cpu_context_t *ctx) {
    /* 参数在栈: [ret] hwnd, msg, wParam, lParam */
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    (void)esp;
    /* DefWindowProcW: 默认返回 0 (除少数消息外) */
    ctx->gpr[CPU_REG_EAX] = 0;
    return 0;
}

/* ---- BeginPaint thunk ----
 * 参数: hwnd, lpPaint (out)
 * Phase 3.1: 分配真实 DC 对象 (gdi32.c), 写入 PAINTSTRUCT.hdc,
 * rcPaint = 客户区 (400x200), 设 g_paint_hdc 供 TextOutW/Rectangle 关联。
 * 旧实现用伪 HDC=1; 现在用 wine_gdi_alloc_dc 返回的句柄。 */
uint32_t user32_BeginPaint_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hwnd = cpu_mem_r32(ctx, esp + 4);
    uint32_t ps_ga = cpu_mem_r32(ctx, esp + 8);

    wine_window_t *w = hwnd_to_win(hwnd);
    uint32_t hdc = wine_gdi_alloc_dc();
    win32_paintstruct_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.hdc = hdc;
    ps.fErase = 1;
    ps.rcPaint_left = 0; ps.rcPaint_top = 0;
    ps.rcPaint_right = 400; ps.rcPaint_bottom = 200;

    uint8_t *p = (uint8_t *)ctx->mem_base + ps_ga;
    memcpy(p, &ps, sizeof(ps));

    g_paint_window = w;
    g_paint_hdc = hdc;
    g_paint_cmd_count = 0;

    ctx->gpr[CPU_REG_EAX] = hdc;
    return hdc;
}

/* ---- EndPaint thunk ----
 * 参数: hwnd, lpPaint
 * Phase 3.1: 调绘制钩子回放命令, 释放 DC 对象, 清 g_paint_window */
void user32_EndPaint_thunk(cpu_context_t *ctx) {
    (void)ctx;
    if (g_paint_hook && g_paint_cmd_count > 0) {
        g_paint_hook(g_paint_cmds, g_paint_cmd_count);
    }
    if (g_paint_hdc) wine_gdi_free_dc(g_paint_hdc);
    g_paint_window = NULL;
    g_paint_hdc = 0;
    g_paint_cmd_count = 0;
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* ---- GetClientRect thunk ---- */
void user32_GetClientRect_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t rc_ga = cpu_mem_r32(ctx, esp + 8);
    win32_rect_t rc = { 0, 0, 400, 200 };
    uint8_t *p = (uint8_t *)ctx->mem_base + rc_ga;
    memcpy(p, &rc, sizeof(rc));
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* ---- 重置 (测试间清状态) ---- */
void wine_window_reset(void) {
    memset(g_classes, 0, sizeof(g_classes));
    memset(g_windows, 0, sizeof(g_windows));
    g_paint_window = NULL;
    g_paint_hdc = 0;
    g_paint_cmd_count = 0;
    g_quit_code = -1;
    /* Phase 3.1: 同步清空 GDI 对象表 (DC/Pen/Brush/Font) */
    wine_gdi_reset();
}
