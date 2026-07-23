# luciswin Phase 3.1 设计文档 — gdi32 对象模型

- **日期**: 2026-07-23
- **状态**: 已批准（待 spec review）
- **范围**: Phase 3.1 —— GDI 对象表 + DC 状态 + 画笔/画刷/字体/位图 + 几何绘制 + ExtTextOut
- **后续 Phase**: 3.2 Shift-JIS 静态表 / 3.3 comctl32 Edit / 3.4 notepad.exe

---

## 1. 背景与范围决策

### 1.1 现状

Phase 2.2 的 gdi32 仅有一个 `gdi32_TextOutW_thunk`，无对象模型：
- 伪 `HDC=1`，无 DC 状态
- 无法 `SelectObject` 选笔/刷/字体
- 无法画几何图形（Rectangle/LineTo/Ellipse）
- paint_hook 命令结构只有 `text/x/y`，无法表达颜色/矩形

### 1.2 Phase 3.1 决策

| 决策项 | 选择 | 理由 |
|---|---|---|
| 句柄模型 | 1-based 32 位 id 映射全局对象表 | 与已有 WINE_FUNC_ID_BASE 思路一致 |
| DC 来源 | BeginPaint 时分配真 DC 对象 | 替换伪 HDC=1 |
| 对象类型 | PEN/BRUSH/FONT/DC 四类 (BITMAP 留位暂不实现) | notepad.exe 用不到位图 |
| 颜色 | 32 位 BGR (0x00BBGGRR) | Win32 COLORREF 一致 |
| paint_hook 扩展 | 命令增加 kind 字段 + 颜色/矩形参数 | 测试可断言多种绘制命令 |
| 字体 | CreateFontIndirectW 仅记录 lfFaceName/lfHeight | 不做字形测量 |
| GetDeviceCaps | 硬编码 LOGPIXELSX=96/HORZRES=400/VERTRES=200 | 与 GetClientRect 默认一致 |

### 1.3 不在 Phase 3.1 范围

- 位图绘制（BitBlt/StretchBlt/CreateBitmap）
- 字形度量（GetTextExtentPoint32）
- 路径/区域/裁剪
- 真正光栅化（仅记录命令，不渲染像素）
- comctl32 / notepad.exe（后续子项目）

---

## 2. 数据结构

### 2.1 GDI 对象表（`WineCore/dlls/gdi32/gdi.c`）

```c
typedef enum {
    GDI_OBJ_PEN = 1,
    GDI_OBJ_BRUSH,
    GDI_OBJ_FONT,
    GDI_OBJ_DC,
} gdi_obj_kind_t;

typedef struct {
    int      used;
    gdi_obj_kind_t kind;
    /* PEN */
    uint32_t pen_color;     /* COLORREF */
    int32_t  pen_style;     /* PS_SOLID=0 等 */
    int32_t  pen_width;
    /* BRUSH */
    uint32_t brush_color;
    int32_t  brush_style;   /* BS_SOLID=0 */
    /* FONT */
    char     font_face[32]; /* lfFaceName, 窄字节 (UTF-16 低字节) */
    int32_t  font_height;
    /* DC */
    uint32_t dc_pen;        /* 当前选中 pen 的 obj id, 0=默认 */
    uint32_t dc_brush;
    uint32_t dc_font;
    uint32_t dc_text_color; /* 默认 0 (黑) */
    uint32_t dc_bk_color;   /* 默认 0xFFFFFF (白) */
    int32_t  dc_bk_mode;    /* TRANSPARENT=1 / OPAQUE=2, 默认 OPAQUE */
    int32_t  dc_cur_x, dc_cur_y;  /* MoveToEx/LineTo 当前位置 */
} gdi_obj_t;

#define WINE_GDI_OBJ_MAX 64
static gdi_obj_t g_gdi_objs[WINE_GDI_OBJ_MAX];
```

句柄 = 槽位 + 1（1-based，0=失败/NULL）。

### 2.2 公开头（`WineCore/include/wine/gdi.h`）

```c
/* 测试与宿主观测用 */
void wine_gdi_reset(void);
int  wine_gdi_obj_count(void);        /* 当前 used 对象数 */
gdi_obj_t *wine_gdi_obj_get(uint32_t h); /* 测试断言用 */
```

### 2.3 paint_hook 命令扩展（`WineCore/include/wine/paint_hook.h`）

```c
typedef enum {
    PAINT_TEXT = 1,
    PAINT_RECTANGLE,
    PAINT_LINE,
} paint_kind_t;

typedef struct {
    int      used;
    paint_kind_t kind;
    int      x, y;            /* TEXT: 文本起点; RECT: 左上; LINE: 起点 */
    int      x2, y2;          /* RECT: 右下; LINE: 终点; TEXT: 未用 */
    uint32_t color;           /* 当前前景色 (笔/文字色) */
    uint32_t fill_color;      /* RECT 填充色 (刷) */
    uint16_t text[128];       /* TEXT 文本 (UTF-16, \0 结尾) */
} wine_paint_cmd_t;
```

旧字段保持向后兼容（`text[128]` 不变），新增 `kind/x2/y2/color/fill_color`。

---

## 3. thunk 实现

### 3.1 对象创建

| thunk | 栈参数 (ESP+4 起) | 返回 EAX |
|---|---|---|
| `gdi32_CreatePenThunk` | fnPenStyle, nWidth, crColor | HGDIOBJ (0=失败) |
| `gdi32_CreateSolidBrushThunk` | crColor | HBRUSH (0=失败) |
| `gdi32_CreateFontIndirectWThunk` | lplf (LOGFONT 指针) | HFONT (0=失败) |

LOGFONT 仅读 lfHeight (offset 0) 与 lfFaceName (offset 28, UTF-16)。

### 3.2 对象选择/删除

| thunk | 参数 | 行为 |
|---|---|---|
| `gdi32_SelectObjectThunk` | hdc, hgdiobj | DC 记录对应类型为当前选中；返回先前对象 (旧 id) |
| `gdi32_DeleteObjectThunk` | hgdiobj | 标 used=0；DC 仍持有该 id 的引用由调用方负责 |

`SelectObject` 按 `obj.kind` 路由到 `dc_pen/dc_brush/dc_font`。

### 3.3 几何绘制

| thunk | 参数 | 行为 |
|---|---|---|
| `gdi32_MoveToExThunk` | hdc, x, y, lpPoint(out) | 设 dc_cur_x/y；旧位置写 lpPoint |
| `gdi32_LineToThunk` | hdc, x, y | 记录 LINE 命令 (cur→x,y)；更新 cur=x,y |
| `gdi32_RectangleThunk` | hdc, left, top, right, bottom | 记录 RECTANGLE 命令 (用 dc_pen 边框色 + dc_brush 填充色) |

### 3.4 文本绘制（扩展）

| thunk | 参数 | 行为 |
|---|---|---|
| `gdi32_ExtTextOutWThunk` | hdc, x, y, fuOptions, lprc, lpString, cbCount, lpDx | 记录 TEXT 命令 (忽略 fuOptions/lprc/lpDx) |
| `gdi32_TextOutWThunk` | hdc, x, y, lpString, cbCount | 调 ExtTextOut 路径（保持已有签名） |

TEXT 命令记录 `dc_text_color` 到 `cmd.color`。

### 3.5 DC 状态

| thunk | 参数 | 行为 |
|---|---|---|
| `gdi32_SetTextColorThunk` | hdc, crColor | 设 dc_text_color；返回旧值 |
| `gdi32_SetBkModeThunk` | hdc, iBkMode | 设 dc_bk_mode；返回旧值 |
| `gdi32_SetBkColorThunk` | hdc, crColor | 设 dc_bk_color；返回旧值 |
| `gdi32_GetDeviceCapsThunk` | hdc, nIndex | 按 nIndex 返回硬编码值 |

GetDeviceCaps 映射：
- `LOGPIXELSX=88` → 96
- `LOGPIXELSY=90` → 96
- `HORZRES=8` → 400
- `VERTRES=10` → 200
- 其余 → 0

### 3.6 BeginPaint 集成

`user32_BeginPaint_thunk` 改为：
1. 分配一个 DC 对象 (`gdi_obj_alloc(GDI_OBJ_DC)`)
2. DC 初始 `dc_text_color=0`, `dc_bk_color=0xFFFFFF`, `dc_bk_mode=2` (OPAQUE), `dc_cur_x=0, dc_cur_y=0`
3. 写入 `ps.hdc = 分配的 DC id`
4. 设 `g_paint_hdc = DC id`（供 TextOutW 等记录命令时关联）

`EndPaint` 行为不变（调钩子回放后清空）。

---

## 4. 测试策略

### 4.1 单元测试（`WineCoreTests/gdi_tests.c`）

直接调用 thunk 函数（构造 cpu_context_t），断言对象表状态：

- `gdi_create_pen_returns_nonzero` — CreatePen 返回非 0 id
- `gdi_create_solid_brush_returns_nonzero`
- `gdi_select_object_records_in_dc` — SelectObject(pen/brush) 后 DC 持有该 id
- `gdi_select_object_returns_previous` — 二次 SelectObject 返回旧 id
- `gdi_delete_object_frees_slot` — DeleteObject 后 used=0
- `gdi_set_text_color_returns_old` — SetTextColor 返回旧值
- `gdi_get_device_caps_logpixels` — 返回 96
- `gdi_movetoex_writes_old_point` — lpPoint 写入旧位置

### 4.2 端到端测试（扩展 `winhello.c`）

`winhello.c` WndProc WM_PAINT 改为绘制：
```c
HPEN pen = CreatePen(PS_SOLID, 1, RGB(255,0,0));      /* 红笔 */
HBRUSH brush = CreateSolidBrush(RGB(0,0,255));        /* 蓝刷 */
SelectObject(ps.hdc, pen);
SelectObject(ps.hdc, brush);
Rectangle(ps.hdc, 10, 10, 100, 60);                  /* 矩形 */
MoveToEx(ps.hdc, 10, 70, NULL);
LineTo(ps.hdc, 200, 70);                              /* 直线 */
SetTextColor(ps.hdc, RGB(0,128,0));                   /* 绿字 */
TextOutW(ps.hdc, 10, 80, L"luciswin gdi32", 12);      /* 文本 */
DeleteObject(pen);
DeleteObject(brush);
```

`winhello_end_to_end_tests.c` 扩展断言：
- paint_hook 收到 >=3 条命令（rect + line + text）
- 第一条是 PAINT_RECTANGLE，边框色=红，填充色=蓝
- 第二条是 PAINT_LINE
- 第三条是 PAINT_TEXT，color=绿，文本含 "luciswin gdi32"

### 4.3 既有测试守卫

所有 Phase 1/2 测试保持不变通过：
- hello.exe (MessageBoxA) — BeginPaint 不涉及，TextOutW 改走 ExtTextOut 路径但命令结构兼容
- fib.exe — 不涉及 GDI
- heap.exe — 不涉及 GDI
- block_cache 单测 — 不涉及 GDI

`winhello_e2e_paint_text` 等既有断言需调整（命令结构变）。

---

## 5. 文件清单

| 文件 | 操作 | 内容 |
|---|---|---|
| `WineCore/include/wine/gdi.h` | 新建 | gdi_obj_t / 公开接口 |
| `WineCore/dlls/gdi32/gdi.c` | 新建 | 对象表 + 所有 gdi32 thunk |
| `WineCore/include/wine/paint_hook.h` | 修改 | paint_kind_t + 扩展 wine_paint_cmd_t |
| `WineCore/dlls/user32/window.c` | 修改 | BeginPaint 分配 DC / EndPaint 释放 |
| `WineCore/core.c` | 修改 | 注册 gdi32 新导出 + include gdi.h |
| `WineCoreTests/gdi_tests.c` | 新建 | 8 个 gdi32 单测 |
| `WineCoreTests/winhello_end_to_end_tests.c` | 修改 | 扩展断言多种绘制命令 |
| `WineCoreTests/build_tests.sh` | 修改 | 加 gdi.c 源 |
| `TestExe/winhello.c` | 修改 | WM_PAINT 绘制 rect+line+text |

---

## 6. 演进路线（后续）

```
Phase 3.1  ← 当前: gdi32 对象模型 + 几何/文本绘制
Phase 3.2  : Shift-JIS 静态映射表 (替换 iconv)
Phase 3.3  : comctl32 + Edit 控件
Phase 3.4  : notepad.exe 端到端
```
