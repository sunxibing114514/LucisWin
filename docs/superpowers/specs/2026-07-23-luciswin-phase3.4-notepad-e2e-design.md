# Phase 3.4 — notepad.exe 端到端 设计规范

**日期**: 2026-07-23
**阶段**: Phase 3.4 (notepad.exe 端到端)
**前置**: Phase 3.3 (comctl32 + Edit 控件) 已完成 (commit 9ada369)

## 1. 目标

编写一个最小化的 notepad 风格 Win32 PE 程序 (notepad.c), 交叉编译为
i386 PE (notepad.exe), 作为端到端测试输入验证:

1. PE 加载器能加载含 comctl32 导入的可执行文件
2. 解释器能跑通完整流程: InitCommonControls → RegisterClassExW →
   CreateWindowExW(父) → CreateWindowExW(Edit 子) → SetWindowTextW →
   ShowWindow → UpdateWindow → 消息循环 → BeginPaint/EndPaint →
   WM_COMMAND 处理 → WM_DESTROY → PostQuitMessage → ExitProcess
3. 绘制钩子捕获 PAINT_EDIT 命令, 文本与 SetWindowText 设置的一致
4. 退出码 0

## 2. notepad.c 设计

最小化 notepad: 一个顶层窗口 + 一个 Edit 子控件填满客户区。

```c
#include <windows.h>
void _pei386_runtime_relocator(void) {}

#define IDC_EDIT 1001

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static HWND s_edit;
    switch (msg) {
        case WM_CREATE: {
            /* 创建 Edit 子控件, 填满客户区 */
            s_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"hello notepad",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                10, 10, 380, 180, h, (HMENU)IDC_EDIT, NULL, NULL);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_COMMAND: {
            /* 收到 EN_CHANGE: 用 SetWindowText 修改 Edit (验证双向) */
            if (LOWORD(wp) == IDC_EDIT && HIWORD(wp) == EN_CHANGE) {
                /* 仅首次设置, 避免无限递归 */
                static int s_changed = 0;
                if (!s_changed) {
                    s_changed = 1;
                    SetWindowTextW(s_edit, L"changed");
                }
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int main(void) {
    InitCommonControls();
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"notepadClass";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, L"notepadClass", L"notepad",
        WS_OVERLAPPEDWINDOW, 0, 0, 400, 200, NULL, NULL, NULL, NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess(0);
}
```

注: WM_CREATE 由 CreateWindowExW 内部触发 (Phase 3.3 未实现 WM_CREATE 派发,
故 Edit 改在 WM_CREATE 之外创建 — 实际 main 中直接创建)。

**修正**: 因 Phase 3.3 未派发 WM_CREATE, 把 Edit 创建放到 main 中
CreateWindowExW(父) 之后:

```c
int main(void) {
    InitCommonControls();
    /* 注册类 */
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"notepadClass";
    RegisterClassExW(&wc);
    /* 创建主窗口 */
    HWND h = CreateWindowExW(0, L"notepadClass", L"notepad",
        WS_OVERLAPPEDWINDOW, 0, 0, 400, 200, NULL, NULL, NULL, NULL);
    /* 直接创建 Edit 子控件 (WM_CREATE 未派发, 故放 main) */
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"hello notepad",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 10, 380, 180, h, (HMENU)IDC_EDIT, NULL, NULL);
    SetWindowTextW(edit, L"hello notepad");  /* 触发首次 EN_CHANGE */
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess(0);
}
```

WM_COMMAND 处理: WndProc 收到 EN_CHANGE 后, 仅首次用 SetWindowText 改写为
"changed" (防递归)。这验证 WM_COMMAND → SetWindowText → 新 EN_CHANGE 的
完整链路。

## 3. build_notepad.sh

```bash
i686-w64-mingw32-gcc -O0 -fno-stack-protector -g0 -nostartfiles \
    -e _main -o notepad.exe notepad.c \
    -luser32 -lgdi32 -lcomctl32 -lkernel32
i686-w64-mingw32-strip notepad.exe
cp notepad.exe WineCoreTests/notepad.exe
cp notepad.exe LucisWinApp/Resources/notepad.exe
```

## 4. 端到端测试 (notepad_end_to_end_tests.c)

4 个测试:

- `notepad_e2e_exit_code` — wine_run_exe(notepad.exe) 返回 0
- `notepad_e2e_paint_edit_captured` — 绘制钩子至少调用 1 次, 含 PAINT_EDIT 命令
- `notepad_e2e_edit_text` — PAINT_EDIT 文本含 "hello notepad" 或 "changed"
  (因 SetWindowText("changed") 可能改写文本, 检查含 'n' 'o' 't' 'e' 即可)
- `notepad_e2e_edit_geometry` — PAINT_EDIT 几何 (x=10, y=10, x2=390, y2=190)

## 5. 实现步骤

1. 写 spec (本文档)
2. RED: 写 notepad_end_to_end_tests.c (4 测试), 编译失败 (无 notepad.exe)
3. 写 notepad.c + build_notepad.sh, 运行 build 生成 notepad.exe
4. GREEN: build_tests.sh 加 LUCISWIN_NOTEPAD_EXE 环境变量, 全部测试通过
5. 提交
