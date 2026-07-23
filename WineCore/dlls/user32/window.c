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
#define WINE_WINDOW_TEXT_MAX 256
typedef struct {
    uint32_t hwnd;          /* 1-based id */
    int      class_idx;     /* 指向 g_classes */
    uint32_t quitted;       /* PostQuitMessage 设置 */
    /* 消息队列 */
    win32_msg_t queue[64];
    int         q_head, q_tail;
    int          used;
    /* Phase 3.3: 窗口属性 (CreateWindowExW 参数存储) */
    uint32_t parent_hwnd;   /* 父窗口 HWND, 0=顶层 */
    uint32_t menu;          /* hMenu; 子窗口时为控制 ID */
    uint32_t style;         /* dwStyle */
    uint32_t ex_style;      /* dwExStyle */
    int      is_edit;       /* 类名 == "Edit" 时置 1 */
    int      is_visible;    /* WS_VISIBLE 风格位 */
    int      x, y, w, h;    /* 窗口几何 */
    uint16_t text[WINE_WINDOW_TEXT_MAX]; /* 窗口文本 (UTF-16, 含 \0) */
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

/* ---- 向窗口队列投递消息 (尾部追加, 队满丢弃) ---- */
static void post_message(wine_window_t *w, uint32_t msg,
                         uint32_t wParam, uint32_t lParam) {
    if (!w) return;
    int next = (w->q_tail + 1) % 64;
    if (next == w->q_head) return;  /* 队满 */
    win32_msg_t *m = &w->queue[w->q_tail];
    memset(m, 0, sizeof(*m));
    m->hwnd = w->hwnd;
    m->message = msg;
    m->wParam = wParam;
    m->lParam = lParam;
    w->q_tail = next;
}

/* ---- Phase 3.3: 预注册系统类 "Edit" ----
 * wnd_proc=0 作哨兵: DispatchMessageW 见 0 不跳 guest, 由宿主代绘
 * (BeginPaint 时为可见 Edit 子窗口发 PAINT_EDIT 命令)。 */
static void register_system_classes(void) {
    for (int i = 0; i < WINE_CLASS_MAX; i++) {
        if (!g_classes[i].used) {
            g_classes[i].name[0] = 'E';
            g_classes[i].name[1] = 'd';
            g_classes[i].name[2] = 'i';
            g_classes[i].name[3] = 't';
            g_classes[i].name[4] = 0;
            g_classes[i].wnd_proc = 0;  /* 哨兵: 宿主代绘 */
            g_classes[i].used = 1;
            return;
        }
    }
}

/* ---- CreateWindowExW thunk ----
 * 参数 (12 个): dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, w, h,
 *              hWndParent, hMenu, hInstance, lpParam
 * 返回 HWND (1-based id, 0=失败)
 * Phase 3.3: 读取全部参数存储到 window 结构, 检测 "Edit" 类,
 *            仅顶层窗口 (非 WS_CHILD) 投递初始 WM_PAINT。 */
uint32_t user32_CreateWindowExW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t ex_style  = cpu_mem_r32(ctx, esp + 4);
    uint32_t class_ga  = cpu_mem_r32(ctx, esp + 8);
    uint32_t name_ga   = cpu_mem_r32(ctx, esp + 12);
    uint32_t style      = cpu_mem_r32(ctx, esp + 16);
    int32_t  cx         = (int32_t)cpu_mem_r32(ctx, esp + 20);
    int32_t  cy         = (int32_t)cpu_mem_r32(ctx, esp + 24);
    int32_t  cw         = (int32_t)cpu_mem_r32(ctx, esp + 28);
    int32_t  ch         = (int32_t)cpu_mem_r32(ctx, esp + 32);
    uint32_t parent     = cpu_mem_r32(ctx, esp + 36);
    uint32_t menu        = cpu_mem_r32(ctx, esp + 40);
    /* hInstance (esp+44), lpParam (esp+48) — Phase 3.3 不用 */

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
    /* Phase 3.3: 存储窗口属性 */
    w->parent_hwnd = parent;
    w->menu = menu;
    w->style = style;
    w->ex_style = ex_style;
    w->x = cx; w->y = cy; w->w = cw; w->h = ch;
    w->is_visible = (style & WS_VISIBLE) ? 1 : 0;
    /* 检测 "Edit" 类 (类名窄字节比较) */
    w->is_edit = (g_classes[class_idx].name[0] == 'E' &&
                  g_classes[class_idx].name[1] == 'd' &&
                  g_classes[class_idx].name[2] == 'i' &&
                  g_classes[class_idx].name[3] == 't' &&
                  g_classes[class_idx].name[4] == 0) ? 1 : 0;
    /* 复制 lpWindowName (UTF-16) 到 w->text[] */
    if (name_ga) {
        const uint16_t *wn = (const uint16_t *)((uint8_t *)ctx->mem_base + name_ga);
        int i = 0;
        for (; i < WINE_WINDOW_TEXT_MAX - 1 && wn[i]; i++) w->text[i] = wn[i];
        w->text[i] = 0;
    }

    /* 仅顶层窗口投递初始 WM_PAINT (子窗口由父窗口 BeginPaint 代绘) */
    if (!(style & WS_CHILD)) {
        post_message(w, WM_PAINT, 0, 0);
    }

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
 * Phase 3.3: 遍历可见 Edit 子窗口, 追加 PAINT_EDIT 命令到 g_paint_cmds
 * (Edit 无 guest WndProc, 由宿主代绘)。 */
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

    /* Phase 3.3: 为可见 Edit 子窗口发 PAINT_EDIT 命令 */
    for (int i = 0; i < WINE_WINDOW_MAX; i++) {
        wine_window_t *c = &g_windows[i];
        if (!c->used || !c->is_edit || !c->is_visible) continue;
        if (c->parent_hwnd != hwnd) continue;
        if (g_paint_cmd_count >= WINE_PAINT_CMD_MAX) break;
        wine_paint_cmd_t *cmd = &g_paint_cmds[g_paint_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->used = 1;
        cmd->kind = PAINT_EDIT;
        cmd->x = c->x;
        cmd->y = c->y;
        cmd->x2 = c->x + c->w;
        cmd->y2 = c->y + c->h;
        cmd->color = 0x00000000;        /* 黑色文字 */
        cmd->fill_color = 0xFFFFFFFF;   /* 白色背景 */
        /* 复制 text[] (UTF-16, 截断到 127) */
        int t = 0;
        for (; t < 127 && c->text[t]; t++) cmd->text[t] = c->text[t];
        cmd->text[t] = 0;
    }

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

/* ---- Phase 3.3: SetWindowTextW thunk ----
 * 参数: hWnd, lpString (UTF-16, 可空)
 * 返回: 成功 1, 失败 0
 * 若 hWnd 是 Edit 且有父窗口, 向父队列投递 WM_COMMAND(EN_CHANGE)。 */
void user32_SetWindowTextW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hwnd = cpu_mem_r32(ctx, esp + 4);
    uint32_t str_ga = cpu_mem_r32(ctx, esp + 8);

    wine_window_t *w = hwnd_to_win(hwnd);
    if (!w) { ctx->gpr[CPU_REG_EAX] = 0; return; }

    /* 复制 UTF-16 文本到 w->text[] */
    if (str_ga) {
        const uint16_t *s = (const uint16_t *)((uint8_t *)ctx->mem_base + str_ga);
        int i = 0;
        for (; i < WINE_WINDOW_TEXT_MAX - 1 && s[i]; i++) w->text[i] = s[i];
        w->text[i] = 0;
    } else {
        w->text[0] = 0;
    }

    /* Edit 子窗口: 投递 WM_COMMAND(EN_CHANGE) 到父窗口 */
    if (w->is_edit && w->parent_hwnd) {
        wine_window_t *parent = hwnd_to_win(w->parent_hwnd);
        if (parent) {
            uint32_t wp = (uint32_t)((w->menu & 0xFFFF) | ((uint32_t)EN_CHANGE << 16));
            post_message(parent, WM_COMMAND, wp, w->hwnd);
        }
    }

    ctx->gpr[CPU_REG_EAX] = 1;
}

/* ---- Phase 3.3: GetWindowTextW thunk ----
 * 参数: hWnd, lpString (out), nMaxCount
 * 返回: 实际复制字符数 (不含 \0) */
void user32_GetWindowTextW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hwnd = cpu_mem_r32(ctx, esp + 4);
    uint32_t buf_ga = cpu_mem_r32(ctx, esp + 8);
    int32_t  maxc = (int32_t)cpu_mem_r32(ctx, esp + 12);

    wine_window_t *w = hwnd_to_win(hwnd);
    if (!w || !buf_ga || maxc <= 0) { ctx->gpr[CPU_REG_EAX] = 0; return; }

    uint16_t *dst = (uint16_t *)((uint8_t *)ctx->mem_base + buf_ga);
    int i = 0;
    for (; i < maxc - 1 && w->text[i]; i++) dst[i] = w->text[i];
    dst[i] = 0;
    ctx->gpr[CPU_REG_EAX] = (uint32_t)i;
}

/* ---- Phase 3.3: GetWindowTextLengthW thunk ----
 * 参数: hWnd
 * 返回: 文本字符数 (不含 \0) */
void user32_GetWindowTextLengthW_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t hwnd = cpu_mem_r32(ctx, esp + 4);

    wine_window_t *w = hwnd_to_win(hwnd);
    if (!w) { ctx->gpr[CPU_REG_EAX] = 0; return; }

    int n = 0;
    while (n < WINE_WINDOW_TEXT_MAX && w->text[n]) n++;
    ctx->gpr[CPU_REG_EAX] = (uint32_t)n;
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
    /* Phase 3.3: 预注册系统类 "Edit" */
    register_system_classes();
}
