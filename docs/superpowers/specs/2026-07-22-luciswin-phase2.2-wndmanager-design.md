# luciswin Phase 2.2 设计规范 — WndManager 完整路径

**状态**: 已批准（用户选择"完整项目"推进 Phase 2）
**日期**: 2026-07-22
**依赖**: Phase 2.1（commit，23/23 测试通过）

---

## 1. 目标与范围

### 1.1 目标

实现真正的 Win32 窗口路径，替代 Phase 1 的 UIAlertController 短路。`winhello.exe`（CreateWindowExW + 消息循环 + WM_PAINT 用 TextOutW 绘制文字）能完整端到端跑通，文字渲染到 iOS 的 UIView/CGContext。

### 1.2 验收标志

`winhello.exe` 端到端：
- RegisterClassExW 注册窗口类
- CreateWindowExW 创建窗口（iOS 侧创建 UIView）
- 消息循环 GetMessage/TranslateMessage/DispatchMessage 转发
- WM_PAINT 调用 TextOutW 画 "luciswin Win32 window"（iOS 侧 setNeedsDisplay 触发 CGContext 绘制）
- WM_DESTROY 收到后 PostQuitMessage 退出循环
- ExitProcess(0)

### 1.3 非目标（留给后续 Phase）

- 多窗口/子窗口/对话框（仅单顶层窗口）
- 完整 GDI（仅 TextOutW + GetClientRect，够 winhello 用）
- 完整键盘/鼠标消息（仅 WM_DESTROY + WM_PAINT + WM_CHAR 占位）
- 滚动条/菜单/加速键
- 字体选择（用系统默认字体）

### 1.4 架构决策

延续 Phase 1/2.1 的 thunk 模式：窗口 API 与 GDI API 都用解释器从 guest 栈读参数、调用 C 实现、返回值写 EAX。窗口对象（HWND）用 32 位 id 映射到 iOS UIView 指针表（类似 WINE_FUNC_ID_BASE 的方案）。渲染在 iOS 侧：WM_PAINT 的 BeginPaint 标记 UIView 脏，TextOutW 记录绘制命令，EndPaint 触发 UIView setNeedsDisplay，UIView 的 drawRect: 回放命令到 CGContext。

---

## 2. 新增组件

### 2.1 user32 窗口 API thunks

| Win32 函数 | 说明 |
|-----------|------|
| RegisterClassExW | 注册窗口类，记类名与 WndProc（guest 函数地址） |
| CreateWindowExW | 创建 UIView，存类名映射，返回 HWND |
| ShowWindow | 空实现（UIView 已显示） |
| UpdateWindow | 触发 setNeedsDisplay |
| GetMessage | 从队列取消息，空队列且无 WM_QUIT 则阻塞（Phase 2.2 用空队列+PostQuitMessage 即可退出） |
| TranslateMessage | 空实现 |
| DispatchMessage | 调用窗口类的 WndProc（guest 函数地址） |
| PostQuitMessage | 设置 WM_QUIT 标志 |
| DefWindowProcW | 默认处理：WM_DESTROY→PostQuitMessage，其余返回 0 |
| BeginPaint | 标记绘制开始，返回 PAINTSTRUCT |
| EndPaint | 结束绘制，触发 setNeedsDisplay |
| GetClientRect | 返回 UIView bounds |

### 2.2 gdi32 绘图 thunk

| Win32 函数 | 说明 |
|-----------|------|
| TextOutW | 记录绘制命令 (x, y, text) 到当前 PAINTSTRUCT 的命令列表 |
| GetTextExtentPoint32W | 估算文本宽高（用 iOS UIFont测量） |

### 2.3 渲染桥接（iOS 侧）

- `WineWindow`（UIView 子类）：持 HWND，drawRect: 回放 PAINTSTRUCT 命令到 CGContext
- `WineWindowManager`（ObjC）：管理 HWND→WineWindow 映射，消息队列，绘制命令缓冲

### 2.4 消息分发机制

解释器 DispatchMessage 调用时：
1. 查 HWND 对应 WineWindow
2. 取窗口类 WndProc（guest 函数地址）
3. 压入参数（hwnd, msg, wParam, lParam）到 guest 栈
4. call WndProc（guest 代码执行，可能再调 BeginPaint/TextOutW/EndPaint/DefWindowProc）
5. ret 后取返回值

关键：WndProc 是 guest 代码，解释器要能 call 到 guest 地址并 ret 回来。这复用 Phase 1 的 `call rel32`/`ret` 机制——但需保证 WndProc 内部调 Win32 API 时 IAT 调用能正常返回。

---

## 3. winhello.exe 测试夹具

```c
#include <windows.h>
void _pei386_runtime_relocator(void) {}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PAINTSTRUCT ps;
    RECT rc;
    switch (msg) {
    case WM_PAINT:
        BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        TextOutW(ps.hdc, 10, 10, L"luciswin Win32 window", 20);
        EndPaint(hwnd, &ps);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"luciswinClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"luciswinClass", L"luciswin",
                                WS_OVERLAPPEDWINDOW, 0, 0, 400, 200,
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
```

注意：winhello.exe 用宽字符 API（W 版本），导入 user32 的 W 函数 + gdi32。编译脚本需加 `-lgdi32`。

### 3.1 验收策略

winhello.exe 在 iOS 真机需 UI 交互，Phase 2.2 的 macOS 测试用钩子验证：
- `wine_set_window_paint_hook` 捕获 TextOutW 的文本
- 验证 GetMessage 返回 0（WM_QUIT）后 wine_run_exe 返回 0
- 验证 WndProc 被调用（WM_PAINT 至少触发一次）

---

## 4. 测试策略（TDD）

| 组 | 文件 | 用例 | 内容 |
|----|------|------|------|
| 窗口单测 | WineCoreTests/window_tests.c（新建） | ~6 | RegisterClass/CreateWindow/GetMessage/Dispatch/DefWindowProc/BeginPaint-EndPaint |
| GDI 单测 | WineCoreTests/gdi_tests.c（新建） | ~2 | TextOutW 命令记录/GetTextExtent |
| winhello 端到端 | WineCoreTests/winhello_end_to_end_tests.c（新建） | 3 | winhello.exe 全跑 |
| 回归 | （已有 23 测试） | 23 | 原测试仍全绿 |

最终预期 ~34 测试全绿。

---

## 5. 文件改动清单

| 文件 | 动作 |
|------|------|
| `WineCore/dlls/user32/window.c` | 新建：窗口 API thunks + 窗口注册表 |
| `WineCore/dlls/user32/user32.c` | 扩展：注册窗口 API 到内建表 |
| `WineCore/dlls/gdi32/gdi32.c` | 新建：TextOutW/GetTextExtent thunks |
| `WineCore/dlls/gdi32/paint_hook.h` | 新建：可插拔绘制钩子接口 |
| `WineCore/include/wine/win32_types.h` | 新建：HWND/HDC/MSG/PAINTSTRUCT 等 32 位类型 |
| `WineCore/core.c` | wine_init 注册 gdi32 |
| `TestExe/winhello.c` + `build_winhello.sh` | 新建 winhello 测试夹具 |
| `WineCoreTests/window_tests.c` | 新建窗口单测 |
| `WineCoreTests/gdi_tests.c` | 新建 GDI 单测 |
| `WineCoreTests/winhello_end_to_end_tests.c` | 新建 winhello 端到端 |
| `WineCoreTests/build_tests.sh` | 加 winhello.exe 路径 |
| `LucisWinApp/WineRunner.m` | 扩展：安装窗口渲染钩子，展示 WineWindow |
| `LucisWinApp/WineWindow.swift`（或 .h/.m） | 新建：UIView 子类 + 窗口管理器 |

---

## 6. 风险与缓解

| 风险 | 缓解 |
|------|------|
| guest WndProc 调用栈深度（DispatchMessage→WndProc→BeginPaint→…） | 复用 Phase 1 call/ret 栈机制，guest 栈足够大（256KB） |
| WM_PAINT 触发死循环（BeginPaint 不清脏标记） | 窗口管理器维护 dirty 标志，BeginPaint 后清，未脏时 GetMessage 不返 WM_PAINT |
| 宽字符 thunk 参数读取 | 复用 Phase 1 cp_to_unicode 思路，W 版本直接读 UTF-16 guest 串 |
| HWND 映射表并发 | Phase 2 单线程，无并发问题 |
| iOS 渲染时序（drawRect 在主线程，解释器在后台） | 绘制命令缓冲到数组，setNeedsDisplay 触发后 drawRect 回放，无需锁 |

---

## 7. 演进路线

```
Phase 2.1  ✓ 指令集扩展 + fibonacci.exe
Phase 2.2  ← 本文档: WndManager 完整路径 + winhello.exe
Phase 2.3  ：TLB / 内存管理 (VirtualAlloc/分页)
Phase 2.4  ：块缓存 (预解码 uops)
```
