/* winhello.c — Phase 2.2 测试夹具
 *
 * 功能: 创建 Win32 窗口, 消息循环, WM_PAINT 用 TextOutW 绘制文字,
 *       WM_DESTROY 后 PostQuitMessage 退出。
 *
 * 验证 luciswin 的完整窗口路径:
 *   RegisterClassExW → CreateWindowExW → ShowWindow → UpdateWindow →
 *   消息循环 (GetMessage/TranslateMessage/DispatchMessage) →
 *   WndProc (BeginPaint/TextOutW/EndPaint/DefWindowProcW) →
 *   WM_DESTROY → PostQuitMessage → ExitProcess
 */
#include <windows.h>

void _pei386_runtime_relocator(void) {}

/* 窗口过程: 处理 WM_PAINT 与 WM_DESTROY */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PAINTSTRUCT ps;
    switch (msg) {
    case WM_PAINT: {
        static const wchar_t text[] = L"luciswin Win32 window";
        BeginPaint(hwnd, &ps);
        TextOutW(ps.hdc, 10, 10, text, 21);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSEXW wc;
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = NULL;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = L"luciswinClass";
    wc.hIconSm = NULL;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"luciswinClass", L"luciswin",
                                WS_OVERLAPPEDWINDOW,
                                0, 0, 400, 200,
                                NULL, NULL, NULL, NULL);
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
