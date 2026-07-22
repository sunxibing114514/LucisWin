# luciswin

iOS 端 Wine 兼容层模拟 — 在未越狱 iOS 设备上运行 Windows EXE。

## Phase 1 范围

端到端跑通 i386 `hello.exe`（Shift-JIS 日文 MessageBoxA），验证主干：

```
PE 加载 → 基址重定位 → IAT 解析 → i386 解释器 → FF /2 IAT 调用
  → MessageBoxA thunk → Shift-JIS→UTF-16 转换 → UIAlertController
  → ExitProcess longjmp → 干净退出
```

- **目标架构**: i386 (32 位 PE)
- **验证手段**: macOS 主机侧单元测试 (20/20)
- **MessageBoxA 路径**: 短路到 UIAlertController（非完整 Win32 窗口）
- **iOS 限制**: 无 JIT，纯解释执行

## 目录结构

```
WineCore/              # 核心静态库 (iOS)
  include/wine/        # 公开头: wine.h, cpu.h, pe_loader.h, codepage.h
  loader/pe_loader.c   # PE32 解析 + 重定位 + 导入解析
  translator/interpreter.c  # i386 switch 解释器 (~30 条指令)
  dlls/kernel32/       # ExitProcess thunk
  dlls/user32/         # MessageBoxA thunk + messagebox_hook.h
  dlls/msvcrt/         # atexit thunk
  codepage/codepage.c  # Shift-JIS → UTF-16LE (iconv)
  core.c              # wine_init / wine_run_exe + 函数指针表
LucisWinApp/           # iOS 应用壳 (Swift + ObjC)
  WineRunner.h/.m     # 加载 hello.exe + UIAlertController 钩子
  MainViewController.swift  # 运行按钮 + 日志
TestExe/               # hello.c + mingw 编译脚本
WineCoreTests/         # macOS 测试: PE/解释器/端到端 (20 用例)
project.yml            # xcodegen 配置
```

## 构建

### 前置依赖

```bash
brew install mingw-w64 xcodegen    # macOS
# apt-get install gcc-mingw-w64-i686 gcc  # Linux (仅 build_tests.sh)
```

### 编译测试夹具

```bash
bash TestExe/build_hello.sh         # 生成 TestExe/hello.exe (i386 PE)
cp TestExe/hello.exe WineCoreTests/hello.exe
```

### 快速测试 (Linux/macOS，无需 Xcode)

```bash
bash WineCoreTests/build_tests.sh
# 期望: 20/20 passed, 0 failed
```

### Xcode 项目 (macOS)

```bash
xcodegen generate
xcodebuild test -scheme WineCoreTests -destination 'platform=macOS'
xcodebuild build -scheme LucisWinApp -sdk iphoneos CODE_SIGNING_REQUIRED=NO
```

## 测试覆盖

| 组 | 文件 | 用例 | 内容 |
|----|------|------|------|
| 解释器 | interpreter_tests.c | 11 | 单指令语义 (mov/add/sub/jcc/call/ret) |
| PE 加载器 | pe_loader_tests.c | 6 | 签名校验/节表/导入表/错误处理 |
| 端到端 | end_to_end_tests.c | 3 | hello.exe 全跑: 文本/caption/flags |

## 关键设计

- **函数指针表**: 64 位宿主函数指针存不进 32 位 IAT 槽，改存 32 位 id（`WINE_FUNC_ID_BASE` 基址），解释器用 `wine_func_get` 反查。
- **IAT 调用两种模式**: `mov eax,[iat]; call eax`（FF /2 检测 id ≥ 基址）与 `call iat_addr`（主循环检测 EIP 落入 IAT 区间）。
- **guest 地址模型**: 重定位 delta = −ImageBase，使 guest 地址 = mem_base 偏移，解释器直接 `mem_base + addr` 访问。
- **thunk 模式**: 解释器从 guest 栈 [ESP+4..] 读 Win32 参数，调用 C 实现，返回值写 EAX。
