/* config.c — persistent configuration for squidget
 *
 * Config file location:
 *   Windows : %APPDATA%\squidget\config
 *   macOS   : ~/.config/squidget/config   (or $XDG_CONFIG_HOME/squidget/config)
 *   Linux   : same as macOS
 *
 * Format (plain text, one key=value per line):
 *   out_dir=/home/alice/Music/squidget
 */

#include "squidget.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/stat.h>        /* mkdir */
#endif

/* ── internal: resolve platform config directory into buf ── */
static void config_dir(char *buf, size_t sz) {
#ifdef _WIN32
    char appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) && appdata[0])
        snprintf(buf, sz, "%s\\squidget", appdata);
    else {
        const char *up = getenv("USERPROFILE");
        snprintf(buf, sz, "%s\\.squidget", up ? up : "C:\\");
    }
#else
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, sz, "%s/squidget", xdg);
    else if (home && *home)
        snprintf(buf, sz, "%s/.config/squidget", home);
    else
        snprintf(buf, sz, "/tmp/squidget_cfg");
#endif
}

/* ── internal: full path to the config file ── */
static void config_file(char *buf, size_t sz) {
    char dir[512];
    config_dir(dir, sizeof(dir));
#ifdef _WIN32
    snprintf(buf, sz, "%s\\config", dir);
#else
    snprintf(buf, sz, "%s/config", dir);
#endif
}

/* ── internal: create the config directory if it doesn't exist ── */
static void ensure_config_dir(void) {
    char dir[512];
    config_dir(dir, sizeof(dir));
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);   /* silently fails if already exists */
#else
    mkdir(dir, 0755);              /* silently fails if already exists */
#endif
}

/* ── public API ──────────────────────────────────────────────────────────
 *
 * config_load: try to read out_dir from the config file.
 *   Returns 1 and fills out_dir[0..sz-1] on success.
 *   Returns 0 if the config file doesn't exist or has no out_dir key.
 */
int config_load(char *out_dir, size_t sz) {
    char path[600];
    config_file(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "out_dir=", 8) != 0) continue;

        char *val = line + 8;
        size_t len = strlen(val);
        /* strip trailing newlines/CR */
        while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r'))
            val[--len] = '\0';

        if (len == 0) continue;   /* ignore blank value */

        snprintf(out_dir, sz, "%s", val);
        fclose(f);
        return 1;
    }

    fclose(f);
    return 0;
}

/*
 * config_save: write (or overwrite) the config file with out_dir.
 */
void config_save(const char *out_dir) {
    ensure_config_dir();

    char path[600];
    config_file(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "out_dir=%s\n", out_dir);
    fclose(f);
}
