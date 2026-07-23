/* notepad.c — Phase 3.4 端到端测试夹具
 *
 * 最小化 notepad: 顶层窗口 + Edit 子控件, 验证 comctl32 + Edit 完整流程。
 *
 * 流程:
 *   InitCommonControls → 注册类 → CreateWindowExW(父) →
 *   CreateWindowExW(Edit 子, "hello notepad") → SetWindowTextW(触发 EN_CHANGE) →
 *   ShowWindow → UpdateWindow → 消息循环 →
 *   WM_PAINT (BeginPaint 发 PAINT_EDIT) → WM_COMMAND (WndProc 处理) →
 *   WM_QUIT → ExitProcess(0)
 *
 * 由 mingw 交叉编译为 i386 PE (notepad.exe), 作为 WineCoreTests 端到端输入。
 */
#include <windows.h>
#include <commctrl.h>

/* mingw -nostartfiles 模式下需要 CRT 重定位器空 stub */
void _pei386_runtime_relocator(void) {}

#define IDC_EDIT 1001

/* 全局计数器: WM_COMMAND 被处理的次数 (guest 内存, 验证 dispatch 走通) */
static int g_cmd_count = 0;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            /* 收到 EN_CHANGE 通知 (SetWindowText 触发), 仅计数避免递归 */
            if (LOWORD(wp) == IDC_EDIT) {
                g_cmd_count++;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    InitCommonControls();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"notepadClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"notepadClass", L"notepad",
                                WS_OVERLAPPEDWINDOW, 0, 0, 400, 200,
                                NULL, NULL, NULL, NULL);

    /* 创建 Edit 子控件, 填满客户区 (WM_CREATE 未派发, 故在 main 创建) */
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"hello notepad",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 10, 10, 380, 180,
                                 hwnd, (HMENU)IDC_EDIT, NULL, NULL);
    /* SetWindowTextW 触发 EN_CHANGE → 投递 WM_COMMAND 到父窗口,
     * 验证 SetWindowTextW thunk + WM_COMMAND 派发链路 */
    SetWindowTextW(edit, L"hello notepad");

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ExitProcess(0);
    return 0;
}
