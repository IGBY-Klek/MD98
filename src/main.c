/* main.c - MD98 main program (Windows 98 compatible Markdown editor)
 *
 * Layout:
 *   - the main window is split into two panes: the left pane is the source
 *     editor (an EDIT control), the right pane is the preview (a RichEdit
 *     control showing the rendered RTF).
 *   - everything uses ANSI (A-suffixed) APIs only; no Unicode dependency.
 *   - file I/O uses the C standard library (fopen/fread/fwrite) and treats
 *     the document as raw ANSI bytes.
 *
 * Windows 98 compatibility:
 *   - no Windows 2000/XP-or-later APIs are used.
 *   - the editor uses the system EDIT control (Win9x ANSI text limit is
 *     about 64KB).
 *   - the preview loads RichEd20.dll first (ships with Win98), falling back
 *     to Riched32.dll (Richedit 1.0).
 *   - all user-visible strings come from the externalized language script
 *     (see lang.c/lang.h); the menu bar is built at run time so the UI can
 *     be switched between English and Chinese (GBK codepage) on the fly.
 */

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* uintptr_t: integer type as wide as a pointer, used as the RichEdit
 * stream callback cookie. C99's stdint.h provides it; MSVC6 (no stdint.h,
 * 32-bit) defines it manually. */
#if defined(_MSC_VER) && (_MSC_VER < 1600)
typedef unsigned long uintptr_t;
#else
#include <stdint.h>
#endif

#include "resource.h"
#include "markdown.h"
#include "render.h"
#include "lang.h"

/* ------------------------------------------------------------------ */
/* RichEdit constants (avoid depending on richedit.h)                   */
/* ------------------------------------------------------------------ */

#ifndef EM_SETBKGNDCOLOR
#define EM_SETBKGNDCOLOR   (WM_USER + 67)
#endif
#ifndef EM_SETTARGETDEVICE
#define EM_SETTARGETDEVICE (WM_USER + 72)
#endif
#ifndef EM_STREAMIN
#define EM_STREAMIN        (WM_USER + 73)
#endif
#ifndef EM_SETOPTIONS
#define EM_SETOPTIONS      (WM_USER + 77)
#endif

#ifndef ECOOP_OR
#define ECOOP_OR           0x0002
#endif
#ifndef ECO_AUTOWORDSELECTION
#define ECO_AUTOWORDSELECTION 0x00000001
#endif
#ifndef ECO_READONLY
#define ECO_READONLY       0x00000800
#endif

#ifndef SF_RTF
#define SF_RTF             2
#endif
#ifndef SFF_SELECTION
#define SFF_SELECTION      0x8000
#endif

/* Classic Win9x OPENFILENAME struct size (avoid modern SDK extra fields). */
#ifndef OPENFILENAME_SIZE_VERSION_400
#define OPENFILENAME_SIZE_VERSION_400 76
#endif

/* Preview refresh debounce timer ID. */
#define TIMER_PREVIEW 1
#define PREVIEW_DELAY_MS 250

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hinst;
static HWND      g_hwndMain;
static HWND      g_hwndEditor;
static HWND      g_hwndPreview;
static HWND      g_hwndSplit;
static HACCEL    g_hAccel;

static char   g_fileName[MAX_PATH];
static char   g_baseDir[MAX_PATH];
static BOOL   g_modified;
static BOOL   g_wordWrap = TRUE;
static BOOL   g_loading  = FALSE;

static HFONT  g_fontEditor;

static HMODULE    g_hRichDll;
static const char *g_richClass = "RichEdit20A";

/* Original window proc of the preview control (subclassing). */
static WNDPROC g_prevProc;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK PreviewProc(HWND, UINT, WPARAM, LPARAM);
static void ResizePanes(void);
static void UpdatePreview(void);
static void UpdateTitle(void);
static BOOL MaybeSave(void);
static BOOL SaveFileDlg(void);
static BOOL SaveFile(const char *path);
static BOOL LoadFile(const char *path);
static void OpenFileDlg(void);
static void ToggleWordWrap(void);
static void CreateEditor(void);
static void InitFonts(void);
static BOOL InitRichEdit(void);
static HMENU BuildMenu(void);
static void RebuildMenu(void);
static void ApplyLanguage(const char *code);

/* ------------------------------------------------------------------ */
/* Fonts                                                                */
/* ------------------------------------------------------------------ */

static void InitFonts(void)
{
    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    int height = -MulDiv(10, dpi, 72);   /* 10pt fixed-pitch font */
    ReleaseDC(NULL, hdc);

    {
        const char *face = "Courier New";
        BYTE charset = ANSI_CHARSET;

        /* On a GBK (Simplified Chinese) system, pick a CJK-capable font so
         * the editor can display Chinese text. */
        if (GetACP() == 936) {
            face = "SimSun";
            charset = GB2312_CHARSET;
        }

        g_fontEditor = CreateFontA(height, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, charset,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, face);
    }
}

/* ------------------------------------------------------------------ */
/* Editor                                                               */
/* ------------------------------------------------------------------ */

static void CreateEditor(void)
{
    DWORD style = WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                  ES_AUTOVSCROLL | ES_NOHIDESEL | ES_WANTRETURN | WS_BORDER;

    if (!g_wordWrap)
        style |= (WS_HSCROLL | ES_AUTOHSCROLL);

    g_hwndEditor = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        style, 0, 0, 0, 0,
        g_hwndMain, (HMENU)IDC_EDITOR, g_hinst, NULL);

    SendMessageA(g_hwndEditor, WM_SETFONT, (WPARAM)g_fontEditor, TRUE);
    SendMessageA(g_hwndEditor, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));

    /* Win9x EDIT controls hold about 64KB of ANSI text; request a larger
     * soft limit anyway. */
    SendMessageA(g_hwndEditor, EM_LIMITTEXT, 0x100000, 0);
}

static void ToggleWordWrap(void)
{
    int len = GetWindowTextLengthA(g_hwndEditor);
    char *text = (char *)malloc(len + 1);

    g_wordWrap = !g_wordWrap;

    g_loading = TRUE;
    if (len > 0) {
        GetWindowTextA(g_hwndEditor, text, len + 1);
    } else {
        text[0] = 0;
    }
    DestroyWindow(g_hwndEditor);
    CreateEditor();
    SetWindowTextA(g_hwndEditor, text);
    g_loading = FALSE;

    free(text);
    ResizePanes();
    RebuildMenu();   /* reflect the Word Wrap check mark */
    SetFocus(g_hwndEditor);
}

/* ------------------------------------------------------------------ */
/* Preview (RichEdit)                                                   */
/* ------------------------------------------------------------------ */

static BOOL InitRichEdit(void)
{
    g_hRichDll = LoadLibraryA("RichEd20.dll");
    if (g_hRichDll) {
        g_richClass = "RichEdit20A";
        return TRUE;
    }
    g_hRichDll = LoadLibraryA("Riched32.dll");
    if (g_hRichDll) {
        g_richClass = "RICHEDIT";
        return TRUE;
    }
    return FALSE;
}

/* Subclassed preview window proc.
 *
 * Bug #3 fix: the preview is a read-only display; it must never own the
 * caret. Without this, clicking the preview would place a blinking caret
 * there. We redirect focus to the editor and ignore mouse clicks so no
 * selection / caret can ever appear. */
static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SETFOCUS:
        SetFocus(g_hwndEditor);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        return 0;
    }
    return CallWindowProcA(g_prevProc, hwnd, msg, wParam, lParam);
}

/* In-memory stream callback used to write RTF into the RichEdit control. */
typedef struct {
    char *data;
    int   len;
    int   pos;
} STREAMIN;

typedef struct {
    uintptr_t dwCookie;
    DWORD     dwError;
    DWORD (CALLBACK *pfnCallback)(uintptr_t, LPBYTE, LONG, LONG *);
} EDITSTREAM;

static DWORD CALLBACK stream_in_cb(uintptr_t cookie, LPBYTE buf, LONG cb, LONG *pcb)
{
    STREAMIN *s = (STREAMIN *)cookie;
    LONG remaining = s->len - s->pos;
    if (remaining <= 0) {
        *pcb = 0;
        return 0;
    }
    {
        LONG n = cb < remaining ? cb : remaining;
        memcpy(buf, s->data + s->pos, n);
        s->pos += n;
        *pcb = n;
    }
    return 0;
}

static void UpdatePreview(void)
{
    int len = GetWindowTextLengthA(g_hwndEditor);
    char *text;
    MD_DOC doc;
    char *rtf;
    int rlen;
    EDITSTREAM es;
    STREAMIN si;

    text = (char *)malloc(len + 1);
    GetWindowTextA(g_hwndEditor, text, len + 1);

    memset(&doc, 0, sizeof(doc));
    md_parse(text, len, &doc);

    rtf = NULL;
    rlen = 0;
    render_rtf(&doc, &rtf, &rlen);

    si.data = rtf;
    si.len = rlen;
    si.pos = 0;

    memset(&es, 0, sizeof(es));
    es.dwCookie = (uintptr_t)&si;
    es.pfnCallback = stream_in_cb;

    /* Select everything, then replace it with the new RTF stream. */
    SendMessageA(g_hwndPreview, EM_SETSEL, 0, (LPARAM)-1);
    SendMessageA(g_hwndPreview, EM_STREAMIN, SF_RTF, (LPARAM)&es);

    free(rtf);
    md_free_doc(&doc);
    free(text);
}

/* ------------------------------------------------------------------ */
/* File operations                                                      */
/* ------------------------------------------------------------------ */

static BOOL SaveFile(const char *path)
{
    int len = GetWindowTextLengthA(g_hwndEditor);
    char *buf = (char *)malloc(len + 1);
    FILE *f;
    char *p;

    GetWindowTextA(g_hwndEditor, buf, len + 1);

    f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return FALSE;
    }

    /* Normalize line endings to CRLF (multi-line EDIT uses CRLF anyway). */
    for (p = buf; *p; p++) {
        if (*p == '\n' && (p == buf || p[-1] != '\r'))
            fputc('\r', f);
        fputc(*p, f);
    }
    fclose(f);
    free(buf);

    strncpy(g_fileName, path, MAX_PATH - 1);
    g_fileName[MAX_PATH - 1] = 0;
    g_modified = FALSE;
    return TRUE;
}

static BOOL LoadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    size_t n;

    if (!f)
        return FALSE;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0)
        size = 0;

    buf = (char *)malloc(size + 1);
    n = fread(buf, 1, (size_t)size, f);
    buf[n] = 0;
    fclose(f);

    g_loading = TRUE;
    SetWindowTextA(g_hwndEditor, buf);
    g_loading = FALSE;
    free(buf);

    strncpy(g_fileName, path, MAX_PATH - 1);
    g_fileName[MAX_PATH - 1] = 0;
    g_modified = FALSE;

    UpdatePreview();
    return TRUE;
}

static BOOL SaveFileDlg(void)
{
    if (g_fileName[0] != 0)
        return SaveFile(g_fileName);

    {
        OPENFILENAMEA ofn;
        char buf[MAX_PATH];
        memset(&ofn, 0, sizeof(ofn));
        wsprintfA(buf, "%s.md", lang_str(L_TITLE_UNTITLED));

        ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400;
        ofn.hwndOwner = g_hwndMain;
        ofn.lpstrFilter = lang_str(L_FILTER);
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = "md";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

        if (!GetSaveFileNameA(&ofn))
            return FALSE;
        return SaveFile(buf);
    }
}

static void OpenFileDlg(void)
{
    OPENFILENAMEA ofn;
    char buf[MAX_PATH];

    if (!MaybeSave())
        return;

    memset(&ofn, 0, sizeof(ofn));
    buf[0] = 0;

    ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400;
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFilter = lang_str(L_FILTER);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn))
        LoadFile(buf);
}

static BOOL MaybeSave(void)
{
    if (!g_modified)
        return TRUE;

    switch (MessageBoxA(g_hwndMain,
        lang_str(L_SAVE_PROMPT),
        lang_str(L_SAVE_TITLE), MB_YESNOCANCEL | MB_ICONQUESTION)) {
    case IDYES:
        return SaveFileDlg();
    case IDNO:
        return TRUE;
    default:
        return FALSE;   /* Cancel */
    }
}

static void NewDocument(void)
{
    if (!MaybeSave())
        return;
    g_loading = TRUE;
    SetWindowTextA(g_hwndEditor, "");
    g_loading = FALSE;
    g_fileName[0] = 0;
    g_modified = FALSE;
    UpdatePreview();
    UpdateTitle();
}

static void UpdateTitle(void)
{
    char title[MAX_PATH + 32];
    wsprintfA(title, lang_str(L_TITLE_FORMAT),
        g_fileName[0] ? g_fileName : lang_str(L_TITLE_UNTITLED));
    SetWindowTextA(g_hwndMain, title);
}

/* ------------------------------------------------------------------ */
/* Menu bar (built at run time from the language script)                */
/* ------------------------------------------------------------------ */

static HMENU BuildMenu(void)
{
    HMENU bar, file, edit, view, lang, help;
    UINT  wwc = g_wordWrap ? MF_CHECKED : MF_UNCHECKED;
    UINT  en  = strcmp(lang_code(), LANG_CODE_EN) == 0 ? MF_CHECKED : MF_UNCHECKED;
    UINT  zh  = strcmp(lang_code(), LANG_CODE_ZH) == 0 ? MF_CHECKED : MF_UNCHECKED;

    bar = CreateMenu();

    file = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, ID_FILE_NEW, lang_str(L_MENU_FILE_NEW));
    AppendMenuA(file, MF_STRING, ID_FILE_OPEN, lang_str(L_MENU_FILE_OPEN));
    AppendMenuA(file, MF_STRING, ID_FILE_SAVE, lang_str(L_MENU_FILE_SAVE));
    AppendMenuA(file, MF_STRING, ID_FILE_SAVEAS, lang_str(L_MENU_FILE_SAVEAS));
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, ID_FILE_EXIT, lang_str(L_MENU_FILE_EXIT));
    AppendMenuA(bar, MF_POPUP, (uintptr_t)file, lang_str(L_MENU_FILE));

    edit = CreatePopupMenu();
    AppendMenuA(edit, MF_STRING, ID_EDIT_UNDO, lang_str(L_MENU_EDIT_UNDO));
    AppendMenuA(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit, MF_STRING, ID_EDIT_CUT, lang_str(L_MENU_EDIT_CUT));
    AppendMenuA(edit, MF_STRING, ID_EDIT_COPY, lang_str(L_MENU_EDIT_COPY));
    AppendMenuA(edit, MF_STRING, ID_EDIT_PASTE, lang_str(L_MENU_EDIT_PASTE));
    AppendMenuA(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit, MF_STRING, ID_EDIT_SELECTALL, lang_str(L_MENU_EDIT_SELECTALL));
    AppendMenuA(bar, MF_POPUP, (uintptr_t)edit, lang_str(L_MENU_EDIT));

    view = CreatePopupMenu();
    AppendMenuA(view, MF_STRING | wwc, ID_VIEW_WORDWRAP, lang_str(L_MENU_VIEW_WORDWRAP));
    AppendMenuA(view, MF_STRING, ID_VIEW_REFRESH, lang_str(L_MENU_VIEW_REFRESH));
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);

    lang = CreatePopupMenu();
    AppendMenuA(lang, MF_STRING | en, ID_LANG_EN, lang_str(L_MENU_LANG_EN));
    AppendMenuA(lang, MF_STRING | zh, ID_LANG_ZH, lang_str(L_MENU_LANG_ZH));
    AppendMenuA(view, MF_POPUP, (uintptr_t)lang, lang_str(L_MENU_LANG));

    AppendMenuA(bar, MF_POPUP, (uintptr_t)view, lang_str(L_MENU_VIEW));

    help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING, ID_HELP_ABOUT, lang_str(L_MENU_HELP_ABOUT));
    AppendMenuA(bar, MF_POPUP, (uintptr_t)help, lang_str(L_MENU_HELP));

    return bar;
}

static void RebuildMenu(void)
{
    HMENU old = GetMenu(g_hwndMain);
    HMENU bar = BuildMenu();
    SetMenu(g_hwndMain, bar);
    if (old)
        DestroyMenu(old);
    DrawMenuBar(g_hwndMain);
}

static void ApplyLanguage(const char *code)
{
    if (strcmp(lang_code(), code) == 0)
        return;
    lang_load_by_code(code, g_baseDir);
    RebuildMenu();
    UpdateTitle();
}

/* ------------------------------------------------------------------ */
/* Layout                                                               */
/* ------------------------------------------------------------------ */

static void ResizePanes(void)
{
    RECT rc;
    int w, h, split, sw;

    GetClientRect(g_hwndMain, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    split = w / 2;
    sw = 4;

    MoveWindow(g_hwndEditor, 0, 0, split - sw / 2, h, TRUE);
    MoveWindow(g_hwndSplit, split - sw / 2, 0, sw, h, TRUE);
    MoveWindow(g_hwndPreview, split + sw / 2, 0, w - split - sw / 2, h, TRUE);
}

/* ------------------------------------------------------------------ */
/* Main window procedure                                                */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        InitRichEdit();
        InitFonts();
        CreateEditor();

        g_hwndSplit = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDVERT,
            0, 0, 0, 0, hwnd, NULL, g_hinst, NULL);

        g_hwndPreview = CreateWindowExA(WS_EX_CLIENTEDGE, g_richClass, "",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_PREVIEW, g_hinst, NULL);

        SendMessageA(g_hwndPreview, EM_SETOPTIONS, ECOOP_OR,
            ECO_AUTOWORDSELECTION | ECO_READONLY);
        SendMessageA(g_hwndPreview, EM_SETBKGNDCOLOR, 0,
            (LPARAM)GetSysColor(COLOR_WINDOW));
        SendMessageA(g_hwndPreview, EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));

        /* Subclass the preview to keep the caret out of it (Bug #3). */
        g_prevProc = (WNDPROC)SetWindowLongA(g_hwndPreview, GWL_WNDPROC,
                                              (LONG)(uintptr_t)PreviewProc);

        ResizePanes();
        SetFocus(g_hwndEditor);
        UpdatePreview();
        return 0;
    }

    /* Bug #2 fix: whenever the main window itself receives focus (initial
     * show, alt-tab back, clicking the background), hand it to the editor.
     * SetFocus() inside WM_CREATE runs before the window is shown and is
     * therefore ineffective, which previously left the focus (and caret) in
     * the preview pane at startup. */
    case WM_SETFOCUS:
        SetFocus(g_hwndEditor);
        return 0;

    case WM_SIZE:
        ResizePanes();
        return 0;

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        /* Editor content changed: mark dirty and schedule a preview refresh. */
        if (id == IDC_EDITOR && code == EN_CHANGE) {
            if (!g_loading) {
                g_modified = TRUE;
                SetTimer(hwnd, TIMER_PREVIEW, PREVIEW_DELAY_MS, NULL);
            }
            return 0;
        }

        switch (id) {
        case ID_FILE_NEW:
            NewDocument();
            break;
        case ID_FILE_OPEN:
            OpenFileDlg();
            UpdateTitle();
            break;
        case ID_FILE_SAVE:
            SaveFileDlg();
            UpdateTitle();
            break;
        case ID_FILE_SAVEAS: {
            OPENFILENAMEA ofn;
            char buf[MAX_PATH];
            memset(&ofn, 0, sizeof(ofn));
            if (g_fileName[0])
                strcpy(buf, g_fileName);
            else
                wsprintfA(buf, "%s.md", lang_str(L_TITLE_UNTITLED));
            ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400;
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = lang_str(L_FILTER);
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = "md";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameA(&ofn)) {
                SaveFile(buf);
                UpdateTitle();
            }
            break;
        }
        case ID_FILE_EXIT:
            SendMessageA(hwnd, WM_CLOSE, 0, 0);
            break;

        case ID_EDIT_UNDO:
            SendMessageA(g_hwndEditor, WM_UNDO, 0, 0);
            break;
        case ID_EDIT_CUT:
            SendMessageA(g_hwndEditor, WM_CUT, 0, 0);
            break;
        case ID_EDIT_COPY:
            SendMessageA(g_hwndEditor, WM_COPY, 0, 0);
            break;
        case ID_EDIT_PASTE:
            SendMessageA(g_hwndEditor, WM_PASTE, 0, 0);
            break;
        case ID_EDIT_SELECTALL:
            SendMessageA(g_hwndEditor, EM_SETSEL, 0, (LPARAM)-1);
            break;

        case ID_VIEW_WORDWRAP:
            ToggleWordWrap();
            break;
        case ID_VIEW_REFRESH:
            UpdatePreview();
            break;

        /* Language switching (external language scripts). */
        case ID_LANG_EN:
            ApplyLanguage(LANG_CODE_EN);
            break;
        case ID_LANG_ZH:
            ApplyLanguage(LANG_CODE_ZH);
            break;

        case ID_HELP_ABOUT:
            MessageBoxA(hwnd,
                lang_str(L_ABOUT_TEXT),
                lang_str(L_ABOUT_TITLE), MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_PREVIEW) {
            KillTimer(hwnd, TIMER_PREVIEW);
            UpdatePreview();
        }
        return 0;

    case WM_CLOSE:
        if (MaybeSave())
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXA wc;
    MSG msg;
    char initialTitle[MAX_PATH + 32];

    (void)hPrevInstance;
    (void)lpCmdLine;
    g_hinst = hInstance;

    /* Language subsystem: detect default from the system codepage and load
     * the matching external language script. */
    lang_init();
    GetModuleFileNameA(hInstance, g_baseDir, MAX_PATH);
    {
        char *s = strrchr(g_baseDir, '\\');
        if (s)
            *s = '\0';
    }
    lang_load_by_code(lang_detect(), g_baseDir);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_APP));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName  = NULL;   /* menu bar is built at run time */
    wc.lpszClassName = "MD98MainWnd";
    wc.hIconSm       = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_APP));

    if (!RegisterClassExA(&wc))
        return 0;

    g_hAccel = LoadAcceleratorsA(hInstance, MAKEINTRESOURCEA(IDR_ACCEL));

    wsprintfA(initialTitle, lang_str(L_TITLE_FORMAT), lang_str(L_TITLE_UNTITLED));

    g_hwndMain = CreateWindowExA(0, wc.lpszClassName,
        initialTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 480,
        NULL, NULL, hInstance, NULL);

    if (!g_hwndMain)
        return 0;

    SetMenu(g_hwndMain, BuildMenu());

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!g_hAccel || !TranslateAcceleratorA(g_hwndMain, g_hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (g_hRichDll)
        FreeLibrary(g_hRichDll);
    if (g_fontEditor)
        DeleteObject(g_fontEditor);

    return (int)msg.wParam;
}
