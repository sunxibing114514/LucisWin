/* codepage.h — 代码页转换接口
 *
 * Phase 1: Shift-JIS (CP932) → UTF-16LE, 使用 iconv 后端
 * Phase 2: 替换为静态映射表 + CoreFoundation
 */
#ifndef WINE_CODEPAGE_H
#define WINE_CODEPAGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CP932 / Shift-JIS 代码页号 */
#define CP_SJIS 932

/* cp_to_unicode: 将多字节字符串转为 UTF-16LE
 *   cp:       代码页 (Phase 1 仅支持 CP_SJIS)
 *   src:      多字节字符串 (不以 \0 结尾也行, 用 src_len 指定长度)
 *   src_len:  src 字节数 (含末尾 \0, 用于转换终止符)
 *   dst:      输出缓冲区 (UTF-16LE)
 *   dst_max:  dst 最大可容纳 uint16_t 个数
 *   返回: 写入 dst 的 UTF-16 字符数 (含终止符), 0 表示失败 */
int cp_to_unicode(int cp, const char *src, size_t src_len,
                  uint16_t *dst, size_t dst_max);

#ifdef __cplusplus
}
#endif

#endif /* WINE_CODEPAGE_H */
