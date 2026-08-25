/* render.h - MD98 Markdown -> RTF 渲染接口 */

#ifndef MD98_RENDER_H
#define MD98_RENDER_H

#include "markdown.h"

/* 将解析后的文档渲染为 RTF（ANSI 编码）文本。
 * 输出缓冲区由本函数分配，调用方使用后应 free(*out)。
 * 返回 0 表示成功。
 */
int render_rtf(const MD_DOC *doc, char **out, int *out_len);

#endif /* MD98_RENDER_H */
