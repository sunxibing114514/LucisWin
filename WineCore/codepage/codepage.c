/* codepage.c — 代码页转换 (Shift-JIS → UTF-16LE)
 *
 * Phase 1: 使用 iconv 后端 (Linux/macOS/iOS 均内置)
 * Phase 2: 替换为静态映射表 + CoreFoundation 后端
 */
#include "wine/codepage.h"
#include <iconv.h>
#include <string.h>
#include <stdio.h>

/* cp_to_unicode: 多字节 → UTF-16LE
 *   src_len 含末尾 \0 (用于转换终止符, 输出含 UTF-16 \0) */
int cp_to_unicode(int cp, const char *src, size_t src_len,
                  uint16_t *dst, size_t dst_max) {
    if (!src || !dst || src_len == 0 || dst_max == 0) return 0;

    /* Phase 1 仅支持 CP_SJIS; 非 Shift-JIS 时做 ASCII 直通 */
    const char *from = NULL;
    if (cp == CP_SJIS) {
        from = "SHIFT_JIS";
    } else {
        /* 回退: 逐字节零扩展 (ASCII 兼容) */
        size_t i;
        for (i = 0; i < src_len && i < dst_max - 1; i++) {
            dst[i] = (uint16_t)(uint8_t)src[i];
        }
        if (i < dst_max) dst[i] = 0;
        return (int)(i + 1);
    }

    iconv_t cd = iconv_open("UTF-16LE", from);
    if (cd == (iconv_t)-1) {
        fprintf(stderr, "[codepage] iconv_open(%s) 失败\n", from);
        return 0;
    }

    char  *inbuf  = (char *)(uintptr_t)src;
    size_t inbytes = src_len;
    char  *outbuf = (char *)(uintptr_t)dst;
    size_t outbytes = dst_max * sizeof(uint16_t);

    size_t r = iconv(cd, &inbuf, &inbytes, &outbuf, &outbytes);
    iconv_close(cd);

    if (r == (size_t)-1) {
        /* 转换出错 (可能含非法序列), 返回已转换部分 */
        /* iconv 在出错前可能已写入部分数据, 计算已写入量 */
    }

    size_t written_bytes = (dst_max * sizeof(uint16_t)) - outbytes;
    int written = (int)(written_bytes / sizeof(uint16_t));
    return written;
}
