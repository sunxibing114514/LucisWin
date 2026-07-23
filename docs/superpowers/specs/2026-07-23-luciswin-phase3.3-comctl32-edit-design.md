# Phase 3.3 — comctl32 + Edit 控件 设计规范

**日期**: 2026-07-23
**阶段**: Phase 3.3 (comctl32 + Edit 控件)
**前置**: Phase 3.1 (gdi32) 已完成 (commit 9223d24), Phase 3.2 (SJIS 表) 已完成 (commit fadc9c0)

## 1. 目标

为 luciswin 增加 Win32 Edit 控件 (单行文本输入) 支持, 作为 Phase 3.4
(notepad.exe 端到端) 的前置能力。具体目标:

1. 注册 `comctl32.dll` 内建导出 (InitCommonControls / InitCommonControlsEx)
2. 预注册 `"Edit"` 系统窗口类
3. 扩展 `CreateWindowExW` 读取 `hWndParent` / `hMenu` / `lpWindowName` / `dwStyle`
4. 实现 `SetWindowTextW` / `GetWindowTextW` / `GetWindowTextLengthW` 文本操作
5. SetWindowTextW 改写 Edit 文本时, 向父窗口投递 WM_COMMAND(EN_CHANGE)
6. 扩展 paint_hook 增加 PAINT_EDIT 命令, BeginPaint 时为可见 Edit 子窗口
   自动发出 PAINT_EDIT 命令 (Edit 无 guest WndProc, 由宿主代绘)

## 2. 数据结构变更

### 2.1 wine_window_t 扩展 (window.c)

```c
typedef struct {
    uint32_t hwnd;          /* 1-based id */
    int      class_idx;     /* 指向 g_classes */
    uint32_t quitted;
    /* 消息队列 */
    win32_msg_t queue[64];
    int         q_head, q_tail;
    int          used;
    /* Phase 3.3 新增字段 */
    uint32_t parent_hwnd;   /* 父窗口 HWND, 0=顶层 */
    uint32_t menu;          /* hMenu, 子窗口时为控制 ID */
    uint32_t style;         /* dwStyle */
    uint32_t ex_style;      /* dwExStyle */
    int      is_edit;       /* 类名 == "Edit" 时置 1 */
    int      is_visible;    /* WS_VISIBLE 风格位 */
    int      x, y, w, h;    /* 窗口几何 (CreateWindowExW 参数) */
    uint16_t text[256];     /* 窗口文本 (UTF-16, 含 \0) */
} wine_window_t;
```

### 2.2 win32_types.h 新增常量

```c
/* 消息 */
#define WM_CREATE        0x0001
#define WM_SETTEXT       0x000C
#define WM_GETTEXT       0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_COMMAND       0x0111
#define WM_KEYDOWN       0x0100
#define WM_CHAR          0x0102

/* 窗口风格 */
#define WS_CHILD         0x40000000u
#define WS_VISIBLE       0x10000000u
#define WS_BORDER        0x00800000u
#define WS_EX_CLIENTEDGE 0x00000200u

/* Edit 风格 */
#define ES_AUTOHSCROLL   0x0080u
#define ES_MULTILINE     0x0004u

/* Edit 通知码 (WM_COMMAND 的 HIWORD(wParam)) */
#define EN_SETFOCUS      0x0100
#define EN_KILLFOCUS     0x0200
#define EN_CHANGE        0x0300
#define EN_UPDATE        0x0400
```

### 2.3 paint_hook.h 扩展

```c
typedef enum {
    PAINT_TEXT = 1,
    PAINT_RECTANGLE,
    PAINT_LINE,
    PAINT_EDIT,    /* Phase 3.3: Edit 控件自动绘制 */
} paint_kind_t;
```

PAINT_EDIT 命令字段语义:
- `x, y` = Edit 左上角 (相对父窗口客户区)
- `x2, y2` = Edit 右下角
- `color` = 文本色 (默认 0=黑色)
- `fill_color` = 背景色 (默认 0xFFFFFFFF=白)
- `text[128]` = UTF-16 文本 (前 127 字符 + \0)

## 3. thunk 实现

### 3.1 CreateWindowExW 扩展 (window.c)

读取全部 12 个 stdcall 参数:

```
[ESP+4]  dwExStyle
[ESP+8]  lpClassName (guest ptr → UTF-16)
[ESP+12] lpWindowName (guest ptr → UTF-16, 可空)
[ESP+16] dwStyle
[ESP+20] x
[ESP+24] y
[ESP+28] nWidth
[ESP+32] nHeight
[ESP+36] hWndParent
[ESP+40] hMenu
[ESP+44] hInstance
[ESP+48] lpParam
```

存储 parent_hwnd / menu / style / ex_style / x / y / w / h。
若 lpWindowName 非空, 复制 UTF-16 文本到 w->text[] (截断到 255)。
若类名为 "Edit", 设 is_edit=1。
若 style 含 WS_VISIBLE, 设 is_visible=1。

仍投递初始 WM_PAINT (与 Phase 2.2 一致)。

### 3.2 SetWindowTextW (user32, 新增)

```
int SetWindowTextW(HWND hWnd, LPCWSTR lpString)
[ESP+4]  hWnd
[ESP+8]  lpString (guest ptr → UTF-16, 可空)
返回: 成功 1, 失败 0
```

- 查窗口; 无效返回 0
- 复制 lpString 到 w->text[] (截断 255, 含 \0)
- 若 w->is_edit 且 w->parent_hwnd 非空: 向父窗口队列投递 WM_COMMAND,
  wParam = MAKELONG(control_id, EN_CHANGE), lParam = hWnd

### 3.3 GetWindowTextW (user32, 新增)

```
int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount)
[ESP+4]  hWnd
[ESP+8]  lpString (guest ptr, 输出缓冲)
[ESP+12] nMaxCount
返回: 实际复制字符数 (不含 \0)
```

- 查窗口; 无效返回 0
- 从 w->text[] 复制到 lpString, 最多 nMaxCount-1 字符 + \0
- 返回复制的字符数 (不含 \0)

### 3.4 GetWindowTextLengthW (user32, 新增)

```
int GetWindowTextLengthW(HWND hWnd)
[ESP+4] hWnd
返回: 文本字符数 (不含 \0)
```

### 3.5 comctl32 InitCommonControls / InitCommonControlsEx (comctl32.dll, 新建)

```
void InitCommonControls(void) — 空实现, "Edit" 已预注册
BOOL InitCommonControlsEx(LPINITCOMMONCONTROLSEX) — 返回 TRUE
```

文件: `WineCore/dlls/comctl32/comctl32.c`

### 3.6 BeginPaint 扩展 (window.c)

在分配 DC、设置 PAINTSTRUCT 后, 遍历 g_windows, 对每个满足
`parent_hwnd == hwnd && is_edit && is_visible` 的子窗口, 追加一条
PAINT_EDIT 命令到 g_paint_cmds:
- x, y = 子窗口 x, y
- x2, y2 = x+w, y+h
- color = 0 (黑)
- fill_color = 0xFFFFFFFF (白)
- text = 子窗口 text[]

(父窗口自身的绘制命令由 guest WndProc 通过 TextOutW 等后续追加。)

## 4. 测试策略

### 4.1 单元测试 (comctl32_tests.c, 直接调用 thunk)

8 个测试:
- `comctl32_init_returns_no_error` — InitCommonControls thunk 调用不崩, EAX=0
- `comctl32_initex_returns_true` — InitCommonControlsEx 返回 TRUE (1)
- `edit_class_preregistered` — wine_window_reset 后用 UTF-16 "Edit" 调 find_class
  (需暴露 find_class 或通过 CreateWindowExW 间接验证), 返回非 0 HWND
- `create_edit_window_returns_nonzero` — CreateWindowExW("Edit", WS_CHILD|WS_VISIBLE,
  parent=1, hMenu=100) 返回非 0
- `set_get_window_text_roundtrip` — SetWindowTextW(h, L"hello"); GetWindowTextW
  读回应得 "hello"
- `get_window_text_length` — GetWindowTextLengthW 返回 5
- `set_text_on_edit_posts_wm_command_to_parent` — 先建 parent (普通类) + Edit 子;
  SetWindowTextW(edit) 后, parent 队列应含 WM_COMMAND, wParam HIWORD=EN_CHANGE,
  LOWORD=control_id, lParam=edit_hwnd
- `beginpaint_emits_paint_edit_for_edit_child` — BeginPaint(parent) 后 g_paint_cmds
  应含一条 PAINT_EDIT, 文本与子 Edit 的 text 一致

### 4.2 端到端测试

Phase 3.4 完成 (notepad.exe)。

## 5. 实现步骤

1. 写 spec (本文档)
2. RED: 写 comctl32_tests.c (8 测试), 编译失败 (缺符号)
3. GREEN:
   a. win32_types.h 加常量
   b. paint_hook.h 加 PAINT_EDIT
   c. window.c 扩展 wine_window_t + 6 个 thunk (CreateWindowExW 重写 + 3 个 text + BeginPaint 扩展) + 预注册 "Edit" 类
   d. dlls/comctl32/comctl32.c 新建
   e. core.c 加 comctl32 extern + 导出表 + wine_init 注册
   f. build_tests.sh 加 comctl32.c 源
4. 验证: bash WineCoreTests/build_tests.sh, 全部测试通过
5. 提交
