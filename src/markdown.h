/* markdown.h - MD98 轻量 Markdown 解析器接口
 *
 * 说明：本解析器面向 Windows 98 环境，仅实现 AI.md 中列出的优先特性，
 * 并不追求完整的 CommonMark/GFM 标准。所有字符串均为 ANSI（单字节/多字节
 * 系统代码页），不使用 Unicode。
 */

#ifndef MD98_MARKDOWN_H
#define MD98_MARKDOWN_H

/* 块类型 */
typedef enum {
    MD_BLOCK_PARAGRAPH,   /* 段落        */
    MD_BLOCK_HEADING,     /* 标题        */
    MD_BLOCK_HR,          /* 水平分割线  */
    MD_BLOCK_CODE,        /* 代码块      */
    MD_BLOCK_UL,          /* 无序列表    */
    MD_BLOCK_OL,          /* 有序列表    */
    MD_BLOCK_QUOTE        /* 引用        */
} MD_BLOCK_TYPE;

/* 一个解析出的块。
 *
 * 对于段落、标题、引用，lines[0] 为拼接后的整段文本（多行以空格连接）。
 * 对于代码块、列表，lines 为逐行文本数组（已去掉前缀符号）。
 */
typedef struct MD_BLOCK {
    MD_BLOCK_TYPE type;
    int  level;          /* 标题级别 1..6，其余块为 0 */
    char **lines;        /* 行数组 */
    int  line_count;
    int  line_cap;
    struct MD_BLOCK *next;
} MD_BLOCK;

typedef struct MD_DOC {
    MD_BLOCK *first;
    MD_BLOCK *last;
} MD_DOC;

/* 解析 Markdown 文本为块链表。text 为 ANSI 文本，len 为字节数。
 * 调用前需将 doc 清零，调用后用 md_free_doc 释放。
 */
void md_parse(const char *text, int len, MD_DOC *doc);

/* 释放 md_parse 产生的所有内存。 */
void md_free_doc(MD_DOC *doc);

#endif /* MD98_MARKDOWN_H */
