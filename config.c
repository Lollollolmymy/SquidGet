// config.c — where we store settings

#include "squidget.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/stat.h>        /* mkdir */
#endif

// get config dir for this os
static void config_dir(char *buf, size_t sz) {
#ifdef _WIN32
    char appdata[1024] = {0};  /* fixed size instead of VLA MAX_PATH */
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) && appdata[0])
        snprintf(buf, sz, "%s\\squidget", appdata);
    else {
        const char *up = getenv("USERPROFILE");
        if (!up) up = "C:\\";
        snprintf(buf, sz, "%s\\.squidget", up);
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

// full path to config file
static void config_file(char *buf, size_t sz) {
    char dir[512];
    config_dir(dir, sizeof(dir));
#ifdef _WIN32
    snprintf(buf, sz, "%s\\config", dir);
#else
    snprintf(buf, sz, "%s/config", dir);
#endif
}

// make config dir if needed
static void ensure_config_dir(void) {
    char dir[512];
    config_dir(dir, sizeof(dir));
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);   /* silently fails if already exists */
#else
    mkdir(dir, 0755);              /* silently fails if already exists */
#endif
}

// load config from disk
int config_load(AppState *s) {
    char path[600];
    config_file(path, sizeof(path));

    s->default_quality = -1; /* default to unset */

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[600];
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "out_dir=", 8) == 0) {
            char *val = line + 8;
            size_t len = strlen(val);
            while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r'))
                val[--len] = '\0';
            if (len > 0) {
                snprintf(s->out_dir, sizeof(s->out_dir), "%s", val);
                loaded = 1;
            }
        } else if (strncmp(line, "quality=", 8) == 0) {
            s->default_quality = atoi(line + 8);
        }
    }

    fclose(f);
    return loaded;
}

// save config to disk (atomic: write to .tmp then rename)
void config_save(AppState *s) {
    ensure_config_dir();

    char path[600];
    config_file(path, sizeof(path));

    char tmp_path[620];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    fprintf(f, "out_dir=%s\n", s->out_dir);
    if (s->default_quality >= 0) {
        fprintf(f, "quality=%d\n", s->default_quality);
    }
    fclose(f);

    /* Atomic replace */
#ifdef _WIN32
    remove(path);
#endif
    rename(tmp_path, path);
}
