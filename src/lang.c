/* lang.c - MD98 externalized language strings implementation
 *
 * Language files are simple plain-text "key = value" lists (one entry per
 * line). Keys are symbolic names; values are the localized strings.
 *
 *   ; a comment line (also '#' works)
 *   key=value
 *
 * The following escape sequences are recognized in values so that a
 * single-line text file can still express special characters:
 *   \t   tab          (menu accelerator column)
 *   \n   newline      (multi-line message box text)
 *   \r   carriage return
 *   \0   NUL byte     (file-dialog filter separators)
 *   \\   backslash
 *
 * The file is read as raw bytes in the system ANSI codepage (GBK on a
 * Simplified-Chinese Windows 98) and passed to the ANSI Win32 APIs without
 * any conversion, matching the Win9x/ANSI design of MD98.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang.h"

/* ------------------------------------------------------------------ */
/* key -> id mapping for the language file format                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *key;
    int         id;
} KEYMAP;

static const KEYMAP keymap[] = {
    { "file",       L_MENU_FILE },
    { "new",        L_MENU_FILE_NEW },
    { "open",       L_MENU_FILE_OPEN },
    { "save",       L_MENU_FILE_SAVE },
    { "saveas",     L_MENU_FILE_SAVEAS },
    { "exit",       L_MENU_FILE_EXIT },
    { "edit",       L_MENU_EDIT },
    { "undo",       L_MENU_EDIT_UNDO },
    { "cut",        L_MENU_EDIT_CUT },
    { "copy",       L_MENU_EDIT_COPY },
    { "paste",      L_MENU_EDIT_PASTE },
    { "selectall",  L_MENU_EDIT_SELECTALL },
    { "view",       L_MENU_VIEW },
    { "wordwrap",   L_MENU_VIEW_WORDWRAP },
    { "refresh",    L_MENU_VIEW_REFRESH },
    { "language",   L_MENU_LANG },
    { "langen",     L_MENU_LANG_EN },
    { "langzh",     L_MENU_LANG_ZH },
    { "help",       L_MENU_HELP },
    { "about",      L_MENU_HELP_ABOUT },
    { "untitled",   L_TITLE_UNTITLED },
    { "titlefmt",   L_TITLE_FORMAT },
    { "saveprompt", L_SAVE_PROMPT },
    { "savetitle",  L_SAVE_TITLE },
    { "abouttitle", L_ABOUT_TITLE },
    { "abouttext",  L_ABOUT_TEXT },
    { "filter",     L_FILTER },
};

/* Built-in English defaults (fallback when no file is loaded or a key is
 * missing from the loaded file). The filter string intentionally contains
 * embedded NUL separators and a final empty terminator as required by the
 * OPENFILENAME dialog. */
static const char *defaults[L_COUNT] = {
    "&File",                                       /* L_MENU_FILE        */
    "&New\tCtrl+N",                                /* L_MENU_FILE_NEW    */
    "&Open...\tCtrl+O",                            /* L_MENU_FILE_OPEN   */
    "&Save\tCtrl+S",                               /* L_MENU_FILE_SAVE   */
    "Save &As...",                                 /* L_MENU_FILE_SAVEAS */
    "E&xit",                                       /* L_MENU_FILE_EXIT   */
    "&Edit",                                       /* L_MENU_EDIT        */
    "&Undo\tCtrl+Z",                               /* L_MENU_EDIT_UNDO   */
    "Cu&t\tCtrl+X",                                /* L_MENU_EDIT_CUT    */
    "&Copy\tCtrl+C",                               /* L_MENU_EDIT_COPY   */
    "&Paste\tCtrl+V",                              /* L_MENU_EDIT_PASTE  */
    "Select &All\tCtrl+A",                         /* L_MENU_EDIT_SELALL */
    "&View",                                       /* L_MENU_VIEW        */
    "&Word Wrap",                                  /* L_MENU_VIEW_WRAP   */
    "&Refresh Preview\tF5",                        /* L_MENU_VIEW_RFRESH */
    "&Language",                                   /* L_MENU_LANG        */
    "English",                                     /* L_MENU_LANG_EN     */
    "Chinese (Simplified)",                        /* L_MENU_LANG_ZH     */
    "&Help",                                       /* L_MENU_HELP        */
    "&About MD98...",                              /* L_MENU_HELP_ABOUT  */
    "Untitled",                                    /* L_TITLE_UNTITLED   */
    "%s - MD98",                                   /* L_TITLE_FORMAT     */
    "The document has been modified. Save changes?",/* L_SAVE_PROMPT      */
    "MD98",                                        /* L_SAVE_TITLE       */
    "About MD98",                                  /* L_ABOUT_TITLE      */
    "MD98 - Markdown editor for Windows 98\n\n"
    "A lightweight Markdown viewer/editor\n"
    "written in C with the Win32 API.",             /* L_ABOUT_TEXT       */
    "Markdown Files (*.md)\0*.md\0Text Files (*.txt)\0*.txt\0"
    "All Files (*.*)\0*.*\0",                       /* L_FILTER           */
};

/* Active strings. NULL means "use the built-in English default". */
static char *strings[L_COUNT];
static char  current_code[8];

static void path_copy(char *dst, int dstlen, const char *src)
{
    if (dstlen <= 0)
        return;
    lstrcpynA(dst, src ? src : "", dstlen);
}

static void path_cat(char *dst, int dstlen, const char *src)
{
    int used;

    if (dstlen <= 0)
        return;
    used = lstrlenA(dst);
    if (used < dstlen - 1)
        lstrcpynA(dst + used, src ? src : "", dstlen - used);
}

static void make_lang_path(char *path, const char *base_dir, const char *mid,
                           const char *code)
{
    path_copy(path, MAX_PATH, base_dir);
    path_cat(path, MAX_PATH, mid);
    path_cat(path, MAX_PATH, code);
    path_cat(path, MAX_PATH, ".lng");
}

/* ------------------------------------------------------------------ */
/* unescape: expand \t \n \r \0 \\ into their byte values              */
/* ------------------------------------------------------------------ */

static char *unescape(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    char *d = out;

    if (!out)
        return NULL;

    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 't':  *d++ = '\t'; break;
            case 'n':  *d++ = '\n'; break;
            case 'r':  *d++ = '\r'; break;
            case '0':  *d++ = '\0'; break;
            case '\\': *d++ = '\\'; break;
            default:   *d++ = '\\'; *d++ = *s; break;
            }
            s++;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* free / init / load                                                  */
/* ------------------------------------------------------------------ */

static void free_all(void)
{
    int i;
    for (i = 0; i < L_COUNT; i++) {
        if (strings[i]) {
            free(strings[i]);
            strings[i] = NULL;
        }
    }
}

void lang_init(void)
{
    free_all();
    strcpy(current_code, LANG_CODE_EN);
}

void lang_set_code(const char *code)
{
    strncpy(current_code, code, sizeof(current_code) - 1);
    current_code[sizeof(current_code) - 1] = '\0';
}

const char *lang_code(void)
{
    return current_code;
}

const char *lang_str(int id)
{
    if (id < 0 || id >= L_COUNT)
        return "";
    return strings[id] ? strings[id] : defaults[id];
}

static void set_string(const char *key, const char *value)
{
    int i;
    int n = (int)(sizeof(keymap) / sizeof(keymap[0]));
    for (i = 0; i < n; i++) {
        if (strcmp(key, keymap[i].key) == 0) {
            char *s = unescape(value);
            if (s) {
                if (strings[keymap[i].id])
                    free(strings[keymap[i].id]);
                strings[keymap[i].id] = s;
            }
            return;
        }
    }
}

static char *trim(char *p)
{
    char *e;
    while (*p == ' ' || *p == '\t')
        p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return p;
}

int lang_load(const char *path)
{
    FILE *f;
    char line[4096];

    f = fopen(path, "rb");
    if (!f)
        return -1;

    /* Only drop the previous strings once the new file is open, so a failed
     * switch (e.g. missing file) keeps the current language intact. */
    free_all();

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        char *eq;
        char *key, *value;
        size_t n = strlen(line);

        /* strip trailing newline / CR */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        p = trim(p);

        /* skip empty lines and comments */
        if (*p == '\0' || *p == ';' || *p == '#')
            continue;

        eq = strchr(p, '=');
        if (!eq)
            continue;

        *eq = '\0';
        key = p;
        value = eq + 1;

        /* trim spaces around the key */
        {
            char *e = key + strlen(key);
            while (e > key && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = '\0';
        }
        value = trim(value);

        set_string(key, value);
    }

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* codepage detection and file path resolution                         */
/* ------------------------------------------------------------------ */

const char *lang_detect(void)
{
    UINT cp = GetACP();
    if (cp == 936)            /* GBK / Simplified Chinese */
        return LANG_CODE_ZH;
    return LANG_CODE_EN;
}

int lang_load_by_code(const char *code, const char *base_dir)
{
    char path[MAX_PATH];

    if (base_dir && base_dir[0]) {
        make_lang_path(path, base_dir, "\\lang\\", code);
        if (lang_load(path) == 0) {
            lang_set_code(code);
            return 0;
        }

        /* If the exe lives in a subdirectory (e.g. bin\), also look in the
         * parent directory's lang\ folder. */
        make_lang_path(path, base_dir, "\\..\\lang\\", code);
        if (lang_load(path) == 0) {
            lang_set_code(code);
            return 0;
        }
    }

    make_lang_path(path, "lang\\", "", code);
    if (lang_load(path) == 0) {
        lang_set_code(code);
        return 0;
    }

    return -1;
}
