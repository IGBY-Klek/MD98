/* markdown.c - MD98 轻量 Markdown 解析器实现
 *
 * 支持特性（对应 AI.md 优先级列表）：
 *   标题(#..######)、段落、水平线、代码块(围栏```与4空格缩进)、
 *   无序/有序列表、引用、以及由渲染层处理的行内语法
 *   （粗体/斜体/删除线/行内代码/链接/图片）。
 *
 * 已知限制：
 *   - 不支持 Setext 标题（==== / ----）。
 *   - 列表不支持嵌套；引用不支持多行 lazy continuation。
 *   - 不解析行内标记（行内标记在 render.c 中处理）。
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "markdown.h"

/* ------------------------------------------------------------------ */
/* 内部工具：动态字符串数组（存放拆分行）                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char **v;
    int    n;
    int    cap;
} LINES;

static void lines_add(LINES *L, const char *s, int len)
{
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 16;
        L->v = (char **)realloc(L->v, nc * sizeof(char *));
        L->cap = nc;
    }
    L->v[L->n] = (char *)malloc(len + 1);
    memcpy(L->v[L->n], s, len);
    L->v[L->n][len] = 0;
    L->n++;
}

static void lines_free(LINES *L)
{
    int i;
    for (i = 0; i < L->n; i++)
        free(L->v[i]);
    free(L->v);
    L->v = NULL;
    L->n = L->cap = 0;
}

/* ------------------------------------------------------------------ */
/* 内部工具：块分配                                                     */
/* ------------------------------------------------------------------ */

static MD_BLOCK *new_block(MD_DOC *doc, MD_BLOCK_TYPE type)
{
    MD_BLOCK *b = (MD_BLOCK *)calloc(1, sizeof(MD_BLOCK));
    if (!b) return NULL;
    b->type = type;
    b->level = 0;
    if (doc->last)
        doc->last->next = b;
    else
        doc->first = b;
    doc->last = b;
    return b;
}

static void block_add_line(MD_BLOCK *b, const char *s, int len)
{
    if (b->line_count == b->line_cap) {
        int nc = b->line_cap ? b->line_cap * 2 : 4;
        b->lines = (char **)realloc(b->lines, nc * sizeof(char *));
        b->line_cap = nc;
    }
    b->lines[b->line_count] = (char *)malloc(len + 1);
    memcpy(b->lines[b->line_count], s, len);
    b->lines[b->line_count][len] = 0;
    b->line_count++;
}

/* 把块的多行合并为单行（用于段落/标题/引用）。 */
static void block_join_lines(MD_BLOCK *b, const char *sep)
{
    int i, total, seplen;
    char *joined, *d;

    if (b->line_count <= 1)
        return;

    seplen = (int)strlen(sep);
    total = 0;
    for (i = 0; i < b->line_count; i++)
        total += (int)strlen(b->lines[i]);
    total += seplen * (b->line_count - 1);

    joined = (char *)malloc(total + 1);
    d = joined;
    for (i = 0; i < b->line_count; i++) {
        int l;
        if (i > 0) {
            memcpy(d, sep, seplen);
            d += seplen;
        }
        l = (int)strlen(b->lines[i]);
        memcpy(d, b->lines[i], l);
        d += l;
    }
    *d = 0;

    for (i = 0; i < b->line_count; i++)
        free(b->lines[i]);
    free(b->lines);

    b->lines = (char **)malloc(sizeof(char *));
    b->lines[0] = joined;
    b->line_count = 1;
    b->line_cap = 1;
}

/* ------------------------------------------------------------------ */
/* 行级判定                                                            */
/* ------------------------------------------------------------------ */

static int is_blank(const char *s)
{
    while (*s) {
        if (!isspace((unsigned char)*s))
            return 0;
        s++;
    }
    return 1;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* 标题：# 开头，1..6 个 #，后跟空格或行尾。返回级别。 */
static int heading_level(const char *s, int *level)
{
    int n = 0;
    while (*s == '#') {
        n++;
        s++;
    }
    if (n >= 1 && n <= 6 && (*s == ' ' || *s == '\t' || *s == 0)) {
        *level = n;
        return 1;
    }
    return 0;
}

/* 水平线：仅由空格与同一符号（-、*、_）构成，且该符号至少 3 个。 */
static int is_hr(const char *s)
{
    const char *p = skip_ws(s);
    char c;
    int n = 0;
    if (!*p) return 0;
    c = *p;
    if (c != '-' && c != '*' && c != '_') return 0;
    while (*p) {
        if (*p == c)
            n++;
        else if (*p != ' ' && *p != '\t')
            return 0;
        p++;
    }
    return n >= 3;
}

static int is_ul(const char *s, const char **rest)
{
    const char *p = skip_ws(s);
    if ((*p == '-' || *p == '*' || *p == '+') &&
        (p[1] == ' ' || p[1] == '\t')) {
        *rest = skip_ws(p + 1);
        return 1;
    }
    return 0;
}

static int is_ol(const char *s, const char **rest)
{
    const char *p = skip_ws(s);
    if (isdigit((unsigned char)*p)) {
        while (isdigit((unsigned char)*p))
            p++;
        if (*p == '.' && (p[1] == ' ' || p[1] == '\t')) {
            *rest = skip_ws(p + 1);
            return 1;
        }
    }
    return 0;
}

static int is_quote(const char *s, const char **rest)
{
    const char *p = skip_ws(s);
    if (*p == '>') {
        p++;
        if (*p == ' ' || *p == '\t')
            p++;
        *rest = p;
        return 1;
    }
    return 0;
}

/* 围栏代码块起始行：``` */
static int is_fence(const char *s, const char **rest)
{
    const char *p = skip_ws(s);
    if (p[0] == '`' && p[1] == '`' && p[2] == '`') {
        *rest = skip_ws(p + 3);
        return 1;
    }
    return 0;
}

static int is_fence_close(const char *s)
{
    const char *p = skip_ws(s);
    return (p[0] == '`' && p[1] == '`' && p[2] == '`');
}

static int is_indented_code(const char *s)
{
    return (s[0] == ' ' && s[1] == ' ' && s[2] == ' ' && s[3] == ' ') ||
           s[0] == '\t';
}

/* 该行是否开启一个新的块（用于判定段落的结束）。 */
static int starts_block(const char *line)
{
    const char *r;
    int lvl;
    return is_blank(line) ||
           heading_level(line, &lvl) ||
           is_hr(line) ||
           is_quote(line, &r) ||
           is_ul(line, &r) ||
           is_ol(line, &r) ||
           is_fence(line, &r) ||
           is_indented_code(line);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void md_parse(const char *text, int len, MD_DOC *doc)
{
    const char *p, *end, *nl, *line_end;
    int i, llen, lvl;
    LINES L;

    doc->first = doc->last = NULL;

    /* 1. 按行拆分（去掉行尾 \r 与空白，便于后续判定）。 */
    L.v = NULL;
    L.n = L.cap = 0;
    p = text;
    end = text + len;
    while (p < end) {
        nl = (const char *)memchr(p, '\n', end - p);
        line_end = nl ? nl : end;
        llen = (int)(line_end - p);
        while (llen > 0 &&
               (p[llen - 1] == '\r' || p[llen - 1] == ' ' || p[llen - 1] == '\t'))
            llen--;
        lines_add(&L, p, llen);
        p = nl ? nl + 1 : end;
    }

    /* 2. 逐行归并为块。 */
    i = 0;
    while (i < L.n) {
        const char *line = L.v[i];
        const char *rest;

        if (is_blank(line)) {
            i++;
            continue;
        }

        /* 围栏代码块 */
        if (is_fence(line, &rest)) {
            MD_BLOCK *b = new_block(doc, MD_BLOCK_CODE);
            i++;
            while (i < L.n) {
                if (is_fence_close(L.v[i])) {
                    i++;
                    break;
                }
                block_add_line(b, L.v[i], (int)strlen(L.v[i]));
                i++;
            }
            continue;
        }

        /* 标题 */
        if (heading_level(line, &lvl)) {
            MD_BLOCK *b = new_block(doc, MD_BLOCK_HEADING);
            const char *h = line + lvl;
            while (*h == ' ' || *h == '\t') h++;
            b->level = lvl;
            block_add_line(b, h, (int)strlen(h));
            i++;
            continue;
        }

        /* 水平线 */
        if (is_hr(line)) {
            new_block(doc, MD_BLOCK_HR);
            i++;
            continue;
        }

        /* 引用 */
        if (is_quote(line, &rest)) {
            MD_BLOCK *b = new_block(doc, MD_BLOCK_QUOTE);
            block_add_line(b, rest, (int)strlen(rest));
            i++;
            while (i < L.n && is_quote(L.v[i], &rest)) {
                block_add_line(b, rest, (int)strlen(rest));
                i++;
            }
            block_join_lines(b, " ");
            continue;
        }

        /* 无序列表 */
        if (is_ul(line, &rest)) {
            MD_BLOCK *b = new_block(doc, MD_BLOCK_UL);
            block_add_line(b, rest, (int)strlen(rest));
            i++;
            while (i < L.n && is_ul(L.v[i], &rest)) {
                block_add_line(b, rest, (int)strlen(rest));
                i++;
            }
            continue;
        }

        /* 有序列表 */
        if (is_ol(line, &rest)) {
            MD_BLOCK *b = new_block(doc, MD_BLOCK_OL);
            block_add_line(b, rest, (int)strlen(rest));
            i++;
            while (i < L.n && is_ol(L.v[i], &rest)) {
                block_add_line(b, rest, (int)strlen(rest));
                i++;
            }
            continue;
        }

        /* 缩进代码块 */
        if (is_indented_code(line)) {
            MD_BLOCK *b = new_block(doc, MD_BLOCK_CODE);
            while (i < L.n && (is_indented_code(L.v[i]) || is_blank(L.v[i]))) {
                if (is_blank(L.v[i]))
                    block_add_line(b, "", 0);
                else
                    block_add_line(b, L.v[i] + 4, (int)strlen(L.v[i]) - 4);
                i++;
            }
            continue;
        }

        /* 段落 */
        {
            MD_BLOCK *b = new_block(doc, MD_BLOCK_PARAGRAPH);
            block_add_line(b, line, (int)strlen(line));
            i++;
            while (i < L.n && !starts_block(L.v[i])) {
                block_add_line(b, L.v[i], (int)strlen(L.v[i]));
                i++;
            }
            block_join_lines(b, " ");
        }
    }

    lines_free(&L);
}

void md_free_doc(MD_DOC *doc)
{
    MD_BLOCK *b = doc->first;
    while (b) {
        MD_BLOCK *next = b->next;
        int i;
        for (i = 0; i < b->line_count; i++)
            free(b->lines[i]);
        free(b->lines);
        free(b);
        b = next;
    }
    doc->first = doc->last = NULL;
}
