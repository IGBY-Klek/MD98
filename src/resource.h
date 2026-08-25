/* resource.h - MD98 resource ID definitions */

#ifndef MD98_RESOURCE_H
#define MD98_RESOURCE_H

/* Resource IDs */
#define IDR_ACCEL               101
#define IDI_APP                 102   /* application icon (asset/MD98.ico) */

/* Child window control IDs */
#define IDC_EDITOR              200
#define IDC_PREVIEW             201

/* Menu command IDs (the menu bar itself is built at run time from the
 * externalized language script - see lang.c / main.c). */
#define ID_FILE_NEW             40001
#define ID_FILE_OPEN            40002
#define ID_FILE_SAVE            40003
#define ID_FILE_SAVEAS          40004
#define ID_FILE_EXIT            40005

#define ID_EDIT_UNDO            40006
#define ID_EDIT_CUT             40007
#define ID_EDIT_COPY            40008
#define ID_EDIT_PASTE           40009
#define ID_EDIT_SELECTALL       40010

#define ID_VIEW_WORDWRAP        40011
#define ID_VIEW_REFRESH         40012

#define ID_HELP_ABOUT           40013

/* Language submenu */
#define ID_LANG_EN              40014
#define ID_LANG_ZH              40015

#endif /* MD98_RESOURCE_H */
