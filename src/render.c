/* render.c - MD98 Markdown -> RTF rendering
 *
 * Renders the parsed blocks to RTF for display in the RichEdit preview.
 *
 * Inline syntax handled in render_inline_span():
 *   **bold**  __bold__        -> \b
 *   *italic*  _italic_        -> \i
 *   ~~strike~~                -> \strike
 *   `inline code`             -> fixed-pitch font
 *   [text](url)               -> blue underlined text
 *   ![alt](url)               -> blue underlined alt text (no image embed)
 *
 * Known limitations (Windows 98 / compatibility related):
 *   - images are not embedded; their alt text is shown as a link.
 *   - inline markup uses greedy matching; full CommonMark semantics are not
 *     implemented.
 *
 * Double-byte (DBCS) support: the whole renderer is DBCS-aware. Multi-byte
 * characters are emitted as a unit, and a trail byte that happens to equal
 * a RTF special ('\', '{', '}') or an inline-markup character is never
 * mistaken for one. This fixes the earlier limitation where a GBK trail
 * byte could break RTF escaping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "render.h"
#include "markdown.h"

/* ------------------------------------------------------------------ */
/* Growable string buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    int   len;
    int   cap;
} RBUF;

static void rbuf_grow(RBUF *b, int extra)
{
    int need = b->len + extra + 1;
    if (need > b->cap) {
        int ncap = b->cap ? b->cap : 256;
        while (ncap < need)
            ncap *= 2;
        b->data = (char *)realloc(b->data, ncap);
        b->cap = ncap;
    }
}

static void rbuf_put(RBUF *b, const char *s, int n)
{
    rbuf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void rbuf_str(RBUF *b, const char *s)
{
    rbuf_put(b, s, (int)strlen(s));
}

/* Append a decimal integer (avoids vsnprintf for MSVC6-era toolchains). */
static void rbuf_int(RBUF *b, int value)
{
    char tmp[16];
    char *p = tmp + sizeof(tmp) - 1;
    int neg = (value < 0);
    unsigned int u = neg ? (unsigned int)(-(value + 1)) + 1u : (unsigned int)value;

    *p = 0;
    do {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (neg)
        *--p = '-';
    rbuf_str(b, p);
}

/* ------------------------------------------------------------------ */
/* DBCS helpers                                                        */
/* ------------------------------------------------------------------ */

/* Is `pos` the trail (second) byte of a double-byte character? `start` must
 * be a character boundary (ASCII byte or DBCS lead byte). Walks left to
 * right, keeping DBCS alignment, so a trail byte is never misread. */
static int is_dbc_trail(const char *start, const char *pos)
{
    const char *p = start;
    while (p < pos) {
        if (IsDBCSLeadByte((BYTE)(unsigned char)*p)) {
            p += 2;
            if (p > pos)
                return 1;
        } else {
            p += 1;
        }
    }
    return 0;
}

/* Emit one ANSI character at s (1 byte, or 2 bytes when s is a DBCS lead
 * byte) with RTF escaping of '\', '{', '}'. Returns bytes consumed (1 or 2).
 * A DBCS pair is emitted under the dedicated CJK font (f2) so Chinese (and
 * other DBCS) text still renders; RichEdit2.0 will not fall back to a CJK
 * font by itself when the run font is an ANSI face. A trail byte equal to a
 * RTF special is never wrongly escaped. */
static int rbuf_char(RBUF *b, const char *s, const char *end)
{
    if (s + 1 < end && IsDBCSLeadByte((BYTE)(unsigned char)*s)) {
        rbuf_str(b, "{\\f2 ");
        rbuf_put(b, s, 2);
        rbuf_str(b, "}");
        return 2;
    }
    if (*s == '\\' || *s == '{' || *s == '}') {
        char c[2] = { '\\', *s };
        rbuf_put(b, c, 2);
    } else {
        rbuf_put(b, s, 1);
    }
    return 1;
}

/* RTF-escape a whole NUL-terminated string, DBCS-aware. */
static void rbuf_escaped(RBUF *b, const char *s)
{
    const char *end = s + strlen(s);
    while (s < end)
        s += rbuf_char(b, s, end);
}

/* ------------------------------------------------------------------ */
/* Inline syntax rendering                                             */
/* ------------------------------------------------------------------ */

/* Find `needle` in [h, hend), ignoring any match whose first byte is the
 * trail byte of a DBCS character (relative to the span start `span`). */
static const char *find_in(const char *span, const char *h,
                           const char *hend, const char *needle)
{
    size_t nlen = strlen(needle);
    while (h + nlen <= hend) {
        if (memcmp(h, needle, nlen) == 0 && !is_dbc_trail(span, h))
            return h;
        h++;
    }
    return NULL;
}

/* Render inline text in [s, e). */
static void render_inline_span(RBUF *b, const char *s, const char *e)
{
    const char *p = s;
    while (p < e) {
        const char *m;

        /* DBCS double-byte character: emit it whole so its trail byte is
         * never mistaken for inline markup below. */
        if (p + 1 < e && IsDBCSLeadByte((BYTE)(unsigned char)*p)) {
            p += rbuf_char(b, p, e);
            continue;
        }

        /* **bold** */
        if (e - p >= 2 && p[0] == '*' && p[1] == '*') {
            m = find_in(s, p + 2, e, "**");
            if (m) {
                rbuf_str(b, "\\b ");
                render_inline_span(b, p + 2, m);
                rbuf_str(b, "\\b0 ");
                p = m + 2;
                continue;
            }
        }
        /* __bold__ */
        if (e - p >= 2 && p[0] == '_' && p[1] == '_') {
            m = find_in(s, p + 2, e, "__");
            if (m) {
                rbuf_str(b, "\\b ");
                render_inline_span(b, p + 2, m);
                rbuf_str(b, "\\b0 ");
                p = m + 2;
                continue;
            }
        }
        /* ~~strike~~ */
        if (e - p >= 2 && p[0] == '~' && p[1] == '~') {
            m = find_in(s, p + 2, e, "~~");
            if (m) {
                rbuf_str(b, "\\strike ");
                render_inline_span(b, p + 2, m);
                rbuf_str(b, "\\strike0 ");
                p = m + 2;
                continue;
            }
        }
        /* `inline code` */
        if (p[0] == '`') {
            m = find_in(s, p + 1, e, "`");
            if (m) {
                rbuf_str(b, "{\\f1 ");
                render_inline_span(b, p + 1, m);
                rbuf_str(b, "}");
                p = m + 1;
                continue;
            }
        }
        /* ![alt](url) image */
        if (p[0] == '!' && p + 1 < e && p[1] == '[') {
            m = find_in(s, p + 2, e, "](");
            if (m) {
                const char *close = find_in(s, m + 2, e, ")");
                if (close) {
                    rbuf_str(b, "{\\cf1\\ul ");
                    render_inline_span(b, p + 2, m);
                    rbuf_str(b, "}");
                    p = close + 1;
                    continue;
                }
            }
        }
        /* [text](url) link */
        if (p[0] == '[') {
            m = find_in(s, p + 1, e, "](");
            if (m) {
                const char *close = find_in(s, m + 2, e, ")");
                if (close) {
                    rbuf_str(b, "{\\cf1\\ul ");
                    render_inline_span(b, p + 1, m);
                    rbuf_str(b, "}");
                    p = close + 1;
                    continue;
                }
            }
        }
        /* *italic* */
        if (p[0] == '*') {
            m = find_in(s, p + 1, e, "*");
            if (m) {
                rbuf_str(b, "\\i ");
                render_inline_span(b, p + 1, m);
                rbuf_str(b, "\\i0 ");
                p = m + 1;
                continue;
            }
        }
        /* _italic_ */
        if (p[0] == '_') {
            m = find_in(s, p + 1, e, "_");
            if (m) {
                rbuf_str(b, "\\i ");
                render_inline_span(b, p + 1, m);
                rbuf_str(b, "\\i0 ");
                p = m + 1;
                continue;
            }
        }

        /* ordinary character (RTF-escaped, DBCS-aware) */
        p += rbuf_char(b, p, e);
    }
}

/* ------------------------------------------------------------------ */
/* Block rendering                                                      */
/* ------------------------------------------------------------------ */

static void render_block(RBUF *b, const MD_BLOCK *blk)
{
    int i;
    switch (blk->type) {
    case MD_BLOCK_PARAGRAPH:
        rbuf_str(b, "\\pard\\fs20 ");
        render_inline_span(b, blk->lines[0], blk->lines[0] + strlen(blk->lines[0]));
        rbuf_str(b, "\\par\n");
        break;

    case MD_BLOCK_HEADING: {
        static const char *sizes[7] = {
            NULL, "\\fs32", "\\fs28", "\\fs24", "\\fs22", "\\fs20", "\\fs20"
        };
        rbuf_str(b, "\\pard\\b ");
        rbuf_str(b, sizes[blk->level]);
        rbuf_str(b, " ");
        render_inline_span(b, blk->lines[0], blk->lines[0] + strlen(blk->lines[0]));
        rbuf_str(b, "\\b0\\fs20\\par\n");
        break;
    }

    case MD_BLOCK_HR:
        /* simulate a horizontal rule with a paragraph bottom border */
        rbuf_str(b, "\\pard\\brdrb\\brdrs\\brdrw10\\par\\par\n");
        break;

    case MD_BLOCK_CODE:
        for (i = 0; i < blk->line_count; i++) {
            rbuf_str(b, "\\pard\\li360\\f1\\fs18 ");
            rbuf_escaped(b, blk->lines[i]);
            rbuf_str(b, "\\par\n");
        }
        rbuf_str(b, "\\pard\\li0\\f0\\fs20\\par\n");
        break;

    case MD_BLOCK_UL:
        for (i = 0; i < blk->line_count; i++) {
            rbuf_str(b, "\\pard\\li360\\fi-360\\bullet\\tab ");
            render_inline_span(b, blk->lines[i], blk->lines[i] + strlen(blk->lines[i]));
            rbuf_str(b, "\\par\n");
        }
        rbuf_str(b, "\\pard\\li0\\par\n");
        break;

    case MD_BLOCK_OL:
        for (i = 0; i < blk->line_count; i++) {
            rbuf_str(b, "\\pard\\li360\\fi-360");
            rbuf_int(b, i + 1);
            rbuf_str(b, ".\\tab ");
            render_inline_span(b, blk->lines[i], blk->lines[i] + strlen(blk->lines[i]));
            rbuf_str(b, "\\par\n");
        }
        rbuf_str(b, "\\pard\\li0\\par\n");
        break;

    case MD_BLOCK_QUOTE:
        rbuf_str(b, "\\pard\\li360\\cf2 ");
        render_inline_span(b, blk->lines[0], blk->lines[0] + strlen(blk->lines[0]));
        rbuf_str(b, "\\cf0\\pard\\li0\\par\n");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public interface                                                     */
/* ------------------------------------------------------------------ */

/* Build the RTF header (fonts, colors). f0/f1 are ANSI (charset 0) faces
 * that carry real bold/italic styles on Windows 98 - the CJK UI fonts (e.g.
 * SimSun) do not have bold/italic faces there, so using them for everything
 * made \b / \i / \strike invisible. f2 is the charset-matched CJK font; CJK
 * runs are switched to it explicitly in rbuf_char(), because RichEdit2.0
 * does not fall back from an ANSI font to a CJK font on its own. */
static void rbuf_header(RBUF *b)
{
    int cp = GetACP();
    const char *cjk_face = "SimSun";
    int cjk_charset = GB2312_CHARSET;

    switch (cp) {
    case 950:  cjk_face = "MingLiU";   cjk_charset = CHINESEBIG5_CHARSET; break;
    case 932:  cjk_face = "MS Gothic"; cjk_charset = SHIFTJIS_CHARSET;    break;
    case 949:  cjk_face = "Gulim";     cjk_charset = HANGUL_CHARSET;      break;
    case 1361: cjk_face = "Gungsuh";   cjk_charset = HANGUL_CHARSET;      break;
    default:   break;   /* 936 and others -> SimSun / GB2312 */
    }

    rbuf_str(b, "{\\rtf1\\ansi\\ansicpg");
    rbuf_int(b, cp);
    rbuf_str(b, "\\deff0\\deflang1033{\\fonttbl");
    rbuf_str(b, "{\\f0\\fswiss\\fcharset0 Arial;}"
                "{\\f1\\fmodern\\fcharset0 Courier New;}"
                "{\\f2\\fnil\\fcharset"); rbuf_int(b, cjk_charset);
    rbuf_str(b, " "); rbuf_str(b, cjk_face); rbuf_str(b, ";}");
    rbuf_str(b, "}{\\colortbl ;\\red0\\green0\\blue255;\\red128\\green128\\blue128;}"
                "\\pard\\f0\\fs20 ");
}

int render_rtf(const MD_DOC *doc, char **out, int *out_len)
{
    RBUF b;
    MD_BLOCK *blk;

    memset(&b, 0, sizeof(b));
    rbuf_header(&b);

    for (blk = doc->first; blk; blk = blk->next)
        render_block(&b, blk);

    rbuf_str(&b, "}");

    *out = b.data;
    *out_len = b.len;
    return 0;
}
