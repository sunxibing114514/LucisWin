/* winhello.c — Phase 2.2 / Phase 3.1 测试夹具
 *
 * 功能: 创建 Win32 窗口, 消息循环, WM_PAINT 用 GDI 对象模型绘制:
 *   - CreatePen/CreateSolidBrush + SelectObject
 *   - Rectangle 矩形 (红边蓝填)
 *   - MoveToEx/LineTo 直线
 *   - SetTextColor + TextOutW 文本 (绿)
 *   - DeleteObject 清理
 *
 * 验证 luciswin 的完整窗口路径 + Phase 3.1 gdi32 对象模型:
 *   RegisterClassExW → CreateWindowExW → ShowWindow → UpdateWindow →
 *   消息循环 (GetMessage/TranslateMessage/DispatchMessage) →
 *   WndProc (BeginPaint/CreatePen/SelectObject/Rectangle/LineTo/TextOutW/
 *            DeleteObject/EndPaint) →
 *   WM_DESTROY → PostQuitMessage → ExitProcess
 */
#include <windows.h>

void _pei386_runtime_relocator(void) {}

/* 窗口过程: 处理 WM_PAINT 与 WM_DESTROY */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PAINTSTRUCT ps;
    switch (msg) {
    case WM_PAINT: {
        BeginPaint(hwnd, &ps);
        /* Phase 3.1: 红笔 + 蓝刷 */
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 255));
        SelectObject(ps.hdc, pen);
        SelectObject(ps.hdc, brush);
        Rectangle(ps.hdc, 10, 10, 100, 60);          /* 红边蓝填矩形 */

        /* 绿色直线 */
        MoveToEx(ps.hdc, 10, 70, NULL);
        LineTo(ps.hdc, 200, 70);

        /* 绿色文本 */
        static const wchar_t text[] = L"luciswin gdi32";
        SetTextColor(ps.hdc, RGB(0, 128, 0));
        TextOutW(ps.hdc, 10, 80, text, 14);

        DeleteObject(pen);
        DeleteObject(brush);
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
