/* platform.c — native graphical folder-picker for squidget
 *
 * gui_pick_folder(buf, bufsz):
 *   Opens the OS-native folder chooser dialog.
 *   Returns 1 and fills buf with the chosen path on success.
 *   Returns 0 if the user cancelled or no GUI is available.
 *
 * Platform implementations:
 *   Windows : PowerShell System.Windows.Forms.FolderBrowserDialog
 *             (always available; no extra dependencies)
 *   macOS   : osascript "choose folder" AppleScript
 *             (always available on macOS 10.x+)
 *   Linux   : tries zenity (GNOME/GTK), then kdialog (KDE/Qt),
 *             then yad; returns 0 if none are installed
 */

#include "squidget.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

// trim trailing whitespace
static size_t strip_nl(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                        s[len-1] == ' '  || s[len-1] == '\t'))
        s[--len] = '\0';
    return len;
}

// ===== windows =====
#ifdef _WIN32
#include <windows.h>

int gui_pick_folder(char *buf, size_t bufsz) {
    // powershell dialog
    const char *ps =
        "powershell -NoProfile -NonInteractive -Command \""
        "Add-Type -AssemblyName System.Windows.Forms; "
        "$d = New-Object System.Windows.Forms.FolderBrowserDialog; "
        "$d.Description = 'Choose where squidget saves music'; "
        "$d.ShowNewFolderButton = $true; "
        "$d.RootFolder = [System.Environment+SpecialFolder]::MyComputer; "
        "if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) "
        "{ Write-Output $d.SelectedPath }\"";

    FILE *p = _popen(ps, "r");
    if (!p) return 0;

    char tmp[512] = {0};
    fgets(tmp, sizeof(tmp), p);
    _pclose(p);

    if (!strip_nl(tmp)) return 0;   /* empty → user cancelled */
    snprintf(buf, bufsz, "%s", tmp);
    return 1;
}

// ===== macos =====
#elif defined(__APPLE__)

int gui_pick_folder(char *buf, size_t bufsz) {
    // osascript dialog
    FILE *p = popen(
        "osascript -e "
        "'POSIX path of (choose folder with prompt "
        "\"Choose where squidget saves music:\")'",
        "r");
    if (!p) return 0;

    char tmp[512] = {0};
    fgets(tmp, sizeof(tmp), p);
    pclose(p);

    size_t len = strip_nl(tmp);
    /* also strip trailing "/" that osascript adds */
    while (len > 1 && tmp[len-1] == '/') tmp[--len] = '\0';

    if (!len) return 0;
    snprintf(buf, bufsz, "%s", tmp);
    return 1;
}

// ===== linux / other posix =====
#else
#include <unistd.h>

// run command & get first line of output
static int popen_first_line(const char *cmd, char *buf, size_t bufsz) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char tmp[512] = {0};
    fgets(tmp, sizeof(tmp), p);
    int status = pclose(p);
    if (WEXITSTATUS(status) != 0) return 0;  /* verify picker succeeded */
    if (!strip_nl(tmp)) return 0;
    snprintf(buf, bufsz, "%s", tmp);
    return 1;
}

int gui_pick_folder(char *buf, size_t bufsz) {
    const char *home  = getenv("HOME");
    const char *start = (home && *home) ? home : "/";

    /* Build a shell-safe version of start: replace every ' with '\'' */
    char safe[768] = {0};
    size_t si = 0;
    for (const char *p = start; *p && si + 5 < sizeof(safe); p++) {
        if (*p == '\'') {
            /* end current quote, literal apostrophe, reopen quote */
            safe[si++] = '\''; safe[si++] = '\\';
            safe[si++] = '\''; safe[si++] = '\'';
        } else {
            safe[si++] = *p;
        }
    }
    safe[si] = '\0';

    char cmd[1024];  /* safe up to 767 chars + longest prefix ~88 chars; was 768 */

    /* ── zenity (GNOME / most GTK desktops) ── */
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --directory"
        " --title='squidget: choose save folder'"
        " --filename='%s/' 2>/dev/null",
        safe);
    if (popen_first_line(cmd, buf, bufsz)) return 1;

    /* ── kdialog (KDE Plasma) ── */
    snprintf(cmd, sizeof(cmd),
        "kdialog --title 'squidget: choose save folder'"
        " --getexistingdirectory '%s' 2>/dev/null",
        safe);
    if (popen_first_line(cmd, buf, bufsz)) return 1;

    /* ── yad (Yet Another Dialog — GTK, often on XFCE/LXDE) ── */
    snprintf(cmd, sizeof(cmd),
        "yad --file --directory"
        " --title='squidget: choose save folder'"
        " --filename='%s/' 2>/dev/null",
        safe);
    if (popen_first_line(cmd, buf, bufsz)) return 1;

    /* No graphical picker found */
    return 0;
}
#endif
