/* hello.c — Phase 1 测试夹具
 *
 * 功能: 调用 MessageBoxA 弹出 Shift-JIS 编码的日文消息
 *
 * 此文件由 mingw (i686-w64-mingw32-gcc) 编译为 i386 PE (hello.exe),
 * 作为 WineCoreTests 的端到端测试输入, 验证 PE 加载器 + 解释器 + Win32 API
 * 调用链完整走通。
 *
 * 字符串字面量使用 \x 转义 Shift-JIS 字节 (CP932), 保证源文件编码无关性:
 *   "こんにちは、iOSのWineです！" -> \x82\xb1\x82\xf1... (Shift-JIS)
 * MessageBoxA 在真实 Windows 上按当前代码页 (通常 CP932) 解释这些字节;
 * 在 luciswin 中由 codepage.c 的 cp_to_unicode(CP_SJIS) 解码为 UTF-16,
 * 再由 platform_show_message_box 钩子展示。
 */

#include <windows.h>

/* mingw -nostartfiles 模式下需要提供 CRT 运行时重定位器的空 stub,
 * 该函数原本用于运行时修复 PE 重定位, Phase 1 解释器已在加载时完成重定位,
 * 故此处空实现即可。链接器据此不再报 undefined reference。 */
void _pei386_runtime_relocator(void) {}

int main(void)
{
    /* Shift-JIS 编码的 "こんにちは、iOSのWineです！"
     * 字节序列由 python3 s.encode('shift_jis') 生成, 见 TestExe/build_hello.sh */
    const char *text =
        "\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd\x81\x41"
        "\x69\x4f\x53\x82\xcc\x57\x69\x6e\x65"
        "\x82\xc5\x82\xb7\x81\x49";
    const char *caption = "luciswin";

    /* 调用 user32!MessageBoxA — 这是端到端测试的关键被拦截点 */
    MessageBoxA(NULL, text, caption, MB_OK);

    /* 调用 kernel32!ExitProcess — 解释器通过 longjmp 跳回 wine_run_exe */
    ExitProcess(0);
    return 0;
}
