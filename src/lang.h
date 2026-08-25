/* lang.h - MD98 externalized language strings interface
 *
 * All user-visible strings live in external language files ("language
 * scripts") stored under a lang/ subdirectory next to the executable.
 *
 * Windows 98 compatibility note: the files are plain text encoded in the
 * system ANSI codepage (e.g. GBK / CP936 on a Simplified-Chinese Win98).
 * The program reads the raw bytes and hands them straight to the ANSI
 * (A-suffixed) Win32 APIs, so no Unicode conversion is required at run
 * time - this is what "rely on the Windows 98 codepage" means here.
 */

#ifndef MD98_LANG_H
#define MD98_LANG_H

/* String identifiers. Keep in sync with lang.c's keymap/defaults. */
enum {
    L_MENU_FILE = 0,
    L_MENU_FILE_NEW,
    L_MENU_FILE_OPEN,
    L_MENU_FILE_SAVE,
    L_MENU_FILE_SAVEAS,
    L_MENU_FILE_EXIT,

    L_MENU_EDIT,
    L_MENU_EDIT_UNDO,
    L_MENU_EDIT_CUT,
    L_MENU_EDIT_COPY,
    L_MENU_EDIT_PASTE,
    L_MENU_EDIT_SELECTALL,

    L_MENU_VIEW,
    L_MENU_VIEW_WORDWRAP,
    L_MENU_VIEW_REFRESH,
    L_MENU_LANG,
    L_MENU_LANG_EN,
    L_MENU_LANG_ZH,

    L_MENU_HELP,
    L_MENU_HELP_ABOUT,

    L_TITLE_UNTITLED,
    L_TITLE_FORMAT,

    L_SAVE_PROMPT,
    L_SAVE_TITLE,

    L_ABOUT_TITLE,
    L_ABOUT_TEXT,

    L_FILTER,

    L_COUNT
};

/* Language codes (also the .lng file base names). */
#define LANG_CODE_EN "en"
#define LANG_CODE_ZH "zh"

/* Initialize with built-in English defaults and no loaded file. */
void lang_init(void);

/* Load one language file (system ANSI codepage). Returns 0 on success,
 * non-zero on failure (e.g. file missing). On failure the built-in
 * English defaults remain active. */
int lang_load(const char *path);

/* Resolve a string ID to the current-language string. Never returns NULL;
 * falls back to the built-in English default for missing entries. */
const char *lang_str(int id);

/* Remember the active language code ("en"/"zh"). */
void lang_set_code(const char *code);

/* Active language code. */
const char *lang_code(void);

/* Auto-detect a default language from the system ANSI codepage:
 * returns "zh" for a GBK (CP936) system, "en" otherwise. */
const char *lang_detect(void);

/* Load the language file for `code` (e.g. "zh") from base_dir\lang\code.lng,
 * falling back to lang\code.lng in the current directory. Returns 0 on
 * success. */
int lang_load_by_code(const char *code, const char *base_dir);

#endif /* MD98_LANG_H */
