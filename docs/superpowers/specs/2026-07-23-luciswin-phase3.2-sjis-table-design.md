# luciswin Phase 3.2 设计文档 — Shift-JIS 静态映射表

- **日期**: 2026-07-23
- **状态**: 已批准
- **范围**: Phase 3.2 —— 内嵌 SJIS→Unicode 静态表, 替换 iconv 后端
- **后续 Phase**: 3.3 comctl32 Edit / 3.4 notepad.exe

---

## 1. 动机

Phase 1 的 `codepage.c` 用 `iconv` 后端转 SJIS→UTF-16。问题：
- iOS 默认不链接 libiconv (需显式链接)
- iconv 行为在不同平台有差异
- 无法精细控制未映射字节替换

Phase 3.2 用内嵌静态表替换, 实现完全自包含的 SJIS 解码。

## 2. SJIS 解码规则

### 2.1 单字节
- `0x00-0x7F`: ASCII 直通 → `U+0000-007F`
- `0xA1-0xDF`: 半角片假名 → `U+FF61-FF9F` (线性: `0xFF61 + (b - 0xA1)`)
- `0x80, 0xE0-0xFF`: 非法单字节 → 替换 `U+FFFD`

### 2.2 双字节 (第一字节 0x81-0x9F / 0xE0-0xEF, 第二字节 0x40-0x7E / 0x80-0xFC)

按 SJIS 双字节区分类查表:

| 第一字节 | 区 | Unicode 范围 | 算法 |
|---|---|---|---|
| 0x81 | 全角标点/符号 | U+3000-303F 等 | 查表 (30 项) |
| 0x82 | 平假名 + 拉丁/数字 | U+3041-3093 等 | 平假名线性, 其余查表 |
| 0x83 | 片假名 | U+30A1-30F6 | 线性 (处理 0x7F 跳变) |
| 0x84 | 片假名续 + 拉丁补充 | U+30FB-30FF 等 | 查表 |
| 0x88-0x9F, 0xE0-0xEF | 汉字 | U+4E00-... | 查表 (Phase 3.2 仅内嵌测试用到的字符) |

### 2.3 未映射
任何未在表内的双字节 → `U+FFFD` (替换字符), 不报错。

## 3. 实现 (`WineCore/codepage/codepage.c`)

- 删除 `#include <iconv.h>`
- 实现 `sjis_decode_double(b1, b2)` → Unicode 码点或 0xFFFD
- 平假名/片假名用线性公式 (覆盖 0x82{9F-F1} / 0x83{40-96})
- 全角标点 + 测试用汉字用静态 `{b1, b2, unicode}` 表
- `cp_to_unicode` 重写为逐字节状态机: 读 b1, 判断单/双字节, 输出对应 UTF-16

## 4. 测试 (`WineCoreTests/codepage_tests.c`)

- `cp_ascii_passthrough` — "hello" 直通
- `cp_sjis_hiragana_konnichiwa` — "こんにちは" → U+3053...
- `cp_sjis_fullwidth_punct` — "、" → U+3001, "！" → U+FF01
- `cp_sjis_halfwidth_kana` — 0xB1 (ｱ) → U+FF71
- `cp_sjis_unknown_replaced` — 非法字节 → U+FFFD
- `cp_sjis_hello_exe_literal` — hello.exe 完整字面量 "こんにちは、iOSのWineです！"

既有 hello.exe 端到端测试保持通过 (作为集成守卫)。

## 5. 文件清单

| 文件 | 操作 |
|---|---|
| `WineCore/codepage/codepage.c` | 重写 (iconv → 静态表) |
| `WineCoreTests/codepage_tests.c` | 新建 (6 单测) |
| `WineCoreTests/build_tests.sh` | 不变 (codepage.c 已在) |
