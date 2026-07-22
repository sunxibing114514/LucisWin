# luciswin Phase 1 设计文档

- **日期**: 2026-07-22
- **状态**: 已批准（待 spec review）
- **范围**: Phase 1 —— 端到端跑通 i386 `hello.exe`（Shift-JIS 日文 MessageBoxA）
- **后续 Phase**: 见末尾"演进路线"

---

## 1. 背景与范围决策

### 1.1 项目背景

`luciswin` 是一个 iOS 端 Wine 兼容层模拟项目，最终目标是在未越狱的 arm64 iOS 设备上运行绝大多数 Windows EXE。完整规范涉及 PE 加载器、x86/x64 解释器、10+ DLL 存根、D3D9→Metal 转译、驱动管理器、插件系统、多版本管理器等子系统，总规模相当于 Wine + box86 + MoltenVK 的组合。

### 1.2 不可调和的技术约束

1. **iOS 无 JIT 限制**: iOS 禁止动态生成可执行代码。"预解码 uops 缓存"只能省去解码开销，解释执行本身仍极慢。复杂 D3D9 游戏在 A14+ 设备上勉强可用。
2. **Apple 沙盒**: 未越狱设备无法 mmap 任意 PE 到指定虚拟地址区间，文件路径需重定向至沙盒内。
3. **单次响应输出能力**: 100+ 文件、累计数万行代码无法在单次响应内保证可编译且无内部矛盾。

### 1.3 Phase 1 决策

基于以上约束，Phase 1 聚焦于验证"PE→解释器→Win32 API→iOS UI"端到端走通的最小可运行骨架：

| 决策项 | 选择 | 理由 |
|---|---|---|
| 验收方式 | 真 x86 解释运行 `hello.exe` | 端到端验证架构走通 |
| 目标架构 | i386 (32 位) | 8 个 GPR，无 REX 前缀，实现量约为 x86_64 的 1/3 |
| 验证手段 | macOS 主机侧单元测试 | 本会话无法运行 iOS 模拟器/设备 |
| MessageBoxA 路径 | 短路到 UIAlertController | 走完整 Win32 窗口路径是 Phase 2 |
| 实现方案 | 方案 A：单 target + `#ifdef` 区分 iOS/macOS | 最小交付量，架构干净 |

### 1.4 不在 Phase 1 范围

- D3D9 / Metal 渲染 / 软件光栅化
- 网络（ws2_32）、音频（dsound）、shell32、ole32、comctl32
- 7z_stub / notepad / regedit 内置工具
- 驱动管理器 UI、插件系统、多版本管理器
- TLB 软件模拟、基本块缓存、背景预翻译（性能优化推迟）
- 完整 Shift-JIS 静态映射表（Phase 1 用 CoreFoundation 后端）
- 完整 x86 指令集（仅实现 hello.exe 用到的 ~30 条）
- 多线程支持（CreateThread / CreateProcess）

---

## 2. 项目结构与构建配置

### 2.1 Phase 1 文件树

```
luciswin/
├── .github/workflows/build.yml         # Phase 1: macOS 编译 + 单测 + iOS xcodebuild
├── WineCore/
│   ├── include/wine/
│   │   ├── pe_loader.h                 # PE 加载器公开 API
│   │   ├── cpu.h                       # cpu_context_t + 解释器入口
│   │   ├── wine.h                      # wine_init/wine_run_exe 公开 API
│   │   └── codepage.h                  # cp_to_unicode/unicode_to_cp
│   ├── loader/pe_loader.c              # PE 解析 + 重定位 + IAT
│   ├── translator/interpreter.c        # computed-goto 主循环
│   ├── translator/cpu_i386_ops.h       # Phase 1 指令表 (~30 条)
│   ├── dlls/ntdll/ntdll.c              # 系统服务 + 虚拟内存 (mmap)
│   ├── dlls/ntdll/ntdll.h
│   ├── dlls/kernel32/kernel32.c        # Heap/Module/Process/Console
│   ├── dlls/kernel32/kernel32.h
│   ├── dlls/user32/user32.c            # MessageBoxA/W + 钩子分发
│   ├── dlls/user32/user32.h
│   ├── dlls/user32/messagebox_hook.h   # platform_show_message_box 声明
│   ├── codepage/codepage.c             # CFString 后端, Phase 2 替换为静态表
│   ├── host/host_messagebox.m          # iOS: UIAlertController; macOS: printf
│   └── core.c                          # wine_init / wine_run_exe 主入口
├── LucisWinApp/
│   ├── AppDelegate.swift               # 初始化 WineWrapper
│   ├── SceneDelegate.swift
│   ├── MainViewController.swift        # "Run hello.exe" 按钮 + 日志视图
│   ├── WineWrapper.h                   # ObjC 桥接 Swift
│   ├── WineWrapper.m
│   ├── HostMessageBoxBridge.m          # 注册 iOS UIAlertController 到 hook
│   ├── Info.plist
│   └── Assets.xcassets/Contents.json
├── TestExe/
│   ├── hello.c                         # MessageBoxA 调用 (Shift-JIS 字面量)
│   ├── test.txt                        # 占位 (Phase 5 才用)
│   └── build_hello.sh                  # mingw 编译脚本 (CI 用)
├── WineCoreTests/                      # macOS 单测 target
│   ├── pe_loader_tests.c               # 用真实 hello.exe 验证解析
│   ├── interpreter_tests.c             # 单条指令单测
│   ├── end_to_end_tests.c              # 跑完 hello.exe, 断言 hook 收到日文
│   └── test_host_messagebox.c          # 捕获 hook 调用
├── project.yml                         # xcodegen 配置
└── README.md                           # Phase 1 范围 + 构建/测试说明
```

### 2.2 `project.yml` 关键配置

- **target `WineCore`** (static library)
  - 源文件: `WineCore/**/*.c|*.m`
  - `deploymentTarget: iOS 15.0`
  - 架构: `arm64`
  - frameworks: `Foundation CoreGraphics UIKit`（Phase 1 不需要 Metal/AudioToolbox）

- **target `WineCoreTests`** (macOS command-line tool, 仅 host CI 跑)
  - 源文件: `WineCoreTests/**/*.c`
  - 依赖 `WineCore`
  - `platform: macOS`
  - 预处理宏: `WINE_HOST_TEST=1`（剥离 UIKit 路径）
  - 链接: `Foundation CoreFoundation`
  - bundle resources: `TestExe/hello.exe`

- **target `LucisWinApp`** (iOS application)
  - 依赖 `WineCore`
  - 源文件: Swift/ObjC
  - resources: `hello.exe`（CI 编译产物）

### 2.3 CI 策略 (`.github/workflows/build.yml`)

`macos-latest` 一个 job，三阶段：

1. `brew install mingw-w64` → `bash TestExe/build_hello.sh` → 生成 `TestExe/hello.exe` 并拷贝到 `LucisWinApp/Resources/` 与 `WineCoreTests/` bundle 资源。
2. `brew install xcodegen` → `xcodegen generate` → `xcodebuild -scheme WineCoreTests -destination 'platform=macOS' test` → 失败则整次失败（这一步证明 hello.exe 真的能跑）。
3. `xcodebuild -scheme LucisWinApp -sdk iphoneos CODE_SIGNING_REQUIRED=NO build` → 上传 IPA artifact。

---

## 3. 架构与数据流

### 3.1 运行时调用链（hello.exe 端到端）

```
wine_run_exe(exe_data, len)
  └─ pe_load_image(exe_data, &image)              // 解析 PE32, mmap 节区, 写 IAT
       └─ pe_resolve_imports(image)               // 递归, 查内建 DLL 导出表
            └─ ntdll/kernel32/user32 的 export 表
  └─ cpu_run(&cpu_ctx, image.entry_point)         // computed-goto 主循环
       ├─ fetch opcode @ RIP
       ├─ goto *dispatch[opcode]
       ├─ 若是 call [IAT slot] → 取出函数指针 → 直接 C 调用
       │    └─ MessageBoxA(hwnd, text, caption, flags)
       │         └─ user32_MessageBoxA: cp_to_unicode(text, CP_SJIS)
       │              └─ platform_show_message_box(wide_text)   ← pluggable hook
       │                   ├─ iOS:  HostMessageBoxBridge → UIAlertController (主线程 dispatch)
       │                   └─ macOS test: test_host_messagebox.c 捕获并断言
       └─ ret → 弹栈回 RIP → 继续 / ExitProcess 则停
```

### 3.2 关键决策：IAT 存 C 函数指针

IAT 槽位存的是 **C 函数指针**（不是 thunk）。解释器遇到 `call dword [addr]` 时，按以下流程处理：

1. 计算 `addr` 的有效地址。
2. 检查 `addr` 是否落在当前 PE image 的 IAT 区间 `[image->iat_base, image->iat_base + image->iat_size)`。
3. 若是：读出槽中的函数指针，按 `__cdecl` 直接 `((win32_func_t)ptr)(args...)` 调用。
4. 若否：按普通间接调用处理（Phase 1 视为 `#UD`，因为没有函数指针表/vtable 的需求）。

这避开在解释器里模拟 Win32 调用约定栈布局的复杂度。hello.exe 用到的所有 Win32 调用都走这条快路径。

为此，`cpu_context_t` 需持有一个指向当前 PE image 的指针（见 §4.1 的 `current_image` 字段），解释器据此判断 IAT 区间。

### 3.3 进程/线程模型（Phase 1 单线程）

- 一个 `wine_process_t`：含 PE image 链表、heap 句柄表、当前线程的 `cpu_context_t`。
- 主线程：`wine_run_exe` 在调用线程上同步跑解释器，直到 `ExitProcess` 或解释器返回。
- `ExitProcess` 实现：setjmp 在 `wine_run_exe` 入口，`ExitProcess` longjmp 跳回并标记退出码。
- 不实现 `CreateThread`/`CreateProcess`（Phase 2）。

---

## 4. 核心数据结构

### 4.1 CPU 上下文 (`WineCore/include/wine/cpu.h`)

```c
typedef struct {
    uint32_t gpr[8];       // EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI (按 ModRM 编码顺序)
    uint32_t eip;
    uint32_t eflags;
    uint16_t seg[6];        // CS,DS,ES,FS,GS,SS — Phase 1 全部 0 (flat mode)
    uint32_t seg_base[6];   // 段基址, 全 0
    uint8_t  fpu_st[8][10]; // 80-bit x87, Phase 1 仅占位
    uint32_t exit_code;     // ExitProcess 写入
    jmp_buf  exit_jmp;      // ExitProcess 跳回
    struct pe_image *current_image;  // 当前 PE image, 解释器据此判断 IAT 区间
    // Phase 2: SSE, TLB, block_cache
} cpu_context_t;
```

### 4.2 PE image (`WineCore/include/wine/pe_loader.h`)

```c
typedef struct pe_image {
    void       *base;          // mmap 基址 (图像基址)
    uint32_t    size;
    uint32_t    entry_point;   // RVA
    void      **iat;           // IAT 槽数组 (函数指针)
    uint32_t    iat_base;      // IAT 起始虚拟地址 (用于解释器区间判断)
    uint32_t    iat_size;      // IAT 字节长度
    char        name[16];      // 模块名
    struct pe_image *next;     // 模块链表
} pe_image_t;

int  pe_load_image(const uint8_t *data, size_t len, pe_image_t *out);
void *pe_resolve_export(pe_image_t *img, const char *name);
```

### 4.3 MessageBoxA 钩子 (`WineCore/dlls/user32/messagebox_hook.h`)

```c
typedef void (*messagebox_hook_t)(const uint16_t *text, const uint16_t *caption,
                                   uint32_t flags, /*out*/ int *result);
void user32_set_messagebox_hook(messagebox_hook_t hook);  // 安装一次, 全局生效
```

---

## 5. 错误处理

| 错误类 | 处理 |
|---|---|
| PE 解析失败 (bad signature) | `pe_load_image` 返回非零, `wine_run_exe` 返回错误码, 不进入解释器 |
| 未解析导入 (DLL/函数名找不到) | IAT 槽写入 `unresolved_import_trap` 函数指针, 调用时打印并 `ExitProcess(1)` |
| 未实现 x86 指令 | `#UD`: 记录 EIP 与 opcode, `longjmp` 到 `exit_jmp`, `exit_code = 0xC000001D` |
| 内存访问越界 (Phase 1) | 简单: SIGSEGV 即崩溃, 不模拟 VAD/页错误 (Phase 2) |
| Heap 分配失败 | `HeapAlloc` 返回 NULL, 不抛异常 |
| MessageBoxA 文本非 Shift-JIS | `cp_to_unicode` 用 `kCFStringEncodingDOSJapanese` 严格模式, 失败字节替换为 U+FFFD |

---

## 6. 测试策略

### 6.1 `WineCoreTests` 三组测试

1. **`pe_loader_tests.c`** — 加载真实 `hello.exe`：
   - 调用 `pe_load_image(hello_exe_bytes, len, &image)` 完成解析与 IAT 填充
   - 断言 `e_lfanew` 正确解析
   - 断言 `Machine == IMAGE_FILE_MACHINE_I386`
   - 断言 `AddressOfEntryPoint` 落在 `.text` 节范围内
   - 断言 IAT 中 `MessageBoxA` 槽非 NULL 且等于 `user32_MessageBoxA` 函数地址
   - 断言导入表包含 `user32.dll` / `kernel32.dll`

2. **`interpreter_tests.c`** — 单指令单测（手工构造 machine code 字节）：
   - `mov eax, 0x12345678` → 断言 `gpr[EAX]==0x12345678`
   - `push 0x10; pop eax` → 断言 `gpr[EAX]==0x10` 且 ESP 复位
   - `xor eax, eax` → 断言 zero flag set
   - `add eax, ebx` + 标志位
   - `jmp rel8` / `jz rel8` 分支
   - `call rel32; ret` 栈平衡

3. **`end_to_end_tests.c`** — 完整跑 hello.exe：
   - 安装测试用 `messagebox_hook_t`：捕获 text/caption/flags 到全局变量
   - 调用 `wine_run_exe(hello_exe_bytes, len)`
   - 断言 hook 被调用一次
   - 断言捕获的 wide text 解码后等于 `こんにちは、iOSのWineです！`
   - 断言 `wine_run_exe` 返回 0

### 6.2 测试资源加载

`hello.exe` 编译到 `WineCoreTests` 的 bundle resource，通过 `[NSBundle mainBundle]` 读取。

### 6.3 CI 中验证

`.github/workflows/build.yml` Step 2 跑 `xcodebuild test -scheme WineCoreTests`，三组测试全过 = Phase 1 验收通过。

---

## 7. Phase 1 指令集覆盖

hello.exe（mingw `-O0` 编译）典型使用到的指令子集，必须在 Phase 1 实现：

| 类别 | 指令 |
|---|---|
| 数据传送 | `mov r/m32, r32` / `mov r32, r/m32` / `mov r32, imm32` / `movzx` / `lea` |
| 栈操作 | `push imm32` / `push r32` / `push r/m32` / `pop r32` / `pop r/m32` / `enter` / `leave` |
| 算术 | `add` / `sub` / `inc` / `dec` / `cmp` |
| 逻辑 | `and` / `or` / `xor` / `test` |
| 控制流 | `jmp rel8/rel32` / `jcc rel8/rel32` (je/jne/jl/jge/jb/ja) / `call rel32` / `call [r/m32]` / `ret` / `ret imm16` |
| IAT 调用 | `call dword [addr]` 特判, 取 IAT 函数指针直接调用 |
| 其它 | `mov [r32], r32` / `mov r32, [r32]` (内存读写) |

未实现指令走 `#UD` 异常路径，记录 EIP 与 opcode 后 `ExitProcess(0xC000001D)`。

---

## 8. 演进路线

仅作设计参考，**不在 Phase 1 实现**：

```
Phase 1  ← 当前：i386 hello.exe + 30 条指令 + 3 个 DLL stub + UIAlertController 短路
Phase 2  ：扩展指令集到完整 8086/386 + SSE2 标量 + TLB + 块缓存 + WndManager 完整路径
Phase 3  ：gdi32 + comctl32 + notepad.exe + 完整 Shift-JIS 静态表
Phase 4  ：d3d9 Metal 驱动 + 软件渲染器 + 简单 HLSL→MSL
Phase 5  ：dsound/ws2_32/shell32/ole32 + 7z_stub + regedit
Phase 6  ：驱动管理器 UI + 插件系统 + 多版本管理器
```

---

## 9. 开放问题（已决策记录）

| # | 问题 | 决策 |
|---|---|---|
| 1 | IAT 存 thunk 还是 C 函数指针 | C 函数指针，解释器直接 `__cdecl` 调用 |
| 2 | Phase 1 是否支持多线程 | 不支持，单线程 |
| 3 | 内存越界处理 | SIGSEGV 崩溃，不模拟 VAD（Phase 2） |
| 4 | 测试覆盖范围 | PE / 解释器单指令 / 端到端三组 |
| 5 | 代码页后端 | Phase 1 用 CoreFoundation, Phase 2 替换为静态表 |
| 6 | 验收命令 | `xcodebuild test -scheme WineCoreTests -destination 'platform=macOS'` |
