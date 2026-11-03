/* audio_backend.c — find and load the engine's audio backend.
 *
 * The Media Kit backend ("mediakit") is built into JackDAW: it needs only the
 * system's libmedia. The JACK backend is an add-on (jack.so), so JackDAW itself does not link
 * libjack and starts on a system without JACK installed. It is looked for in
 * this order, and the first one that loads is used for the rest of the run
 * (it is never unloaded: the engine holds its function table):
 *   1. $JACKDAW_BACKEND_DIR/jack.so, for testing a build
 *   2. backends/jack.so next to the JackDAW executable (a build tree)
 *   3. JackDAW/jack.so in the add-ons directories, user before system,
 *      non-packaged before packaged (the usual Haiku add-on search order)
 *
 * Another application that builds these backends in (VST3-renku's hosts,
 * NAMku) calls audio_backend_load_for with its own name and variable, so it
 * looks for <name>/jack.so and $<variable>/jack.so instead. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FindDirectory.h>
#include <OS.h>
#include <StorageDefs.h>
#include <image.h>

#include "audio_backend.h"

#define BACKEND_FILE "jack.so"

/* Try one path. Returns the table, or NULL with *why set to the first real
 * failure (a file that exists but will not load says more than "not found"). */
static const JdBackend *try_load(const char *path, status_t *why)
{
    image_id img = load_add_on(path);
    if (img < 0) {
        if (img != B_ENTRY_NOT_FOUND && *why == B_ENTRY_NOT_FOUND)
            *why = img;
        return NULL;
    }
    JdBackendEntryFn entry = NULL;
    if (get_image_symbol(img, JD_BACKEND_ENTRY_SYMBOL, B_SYMBOL_TYPE_TEXT, (void **)&entry) !=
            B_OK ||
        !entry) {
        unload_add_on(img);
        if (*why == B_ENTRY_NOT_FOUND)
            *why = B_BAD_VALUE;
        return NULL;
    }
    const JdBackend *b = entry(JD_BACKEND_ABI);
    if (!b) {
        unload_add_on(img);
        if (*why == B_ENTRY_NOT_FOUND)
            *why = B_MISMATCHED_VALUES;
        return NULL;
    }
    return b;
}

/* Try dir + rest. A path that does not fit is skipped, not truncated. */
static const JdBackend *try_path(const char *dir, const char *rest, status_t *why)
{
    char path[B_PATH_NAME_LENGTH];
    int n = snprintf(path, sizeof path, "%s%s", dir, rest);
    if (n < 0 || (size_t)n >= sizeof path)
        return NULL;
    return try_load(path, why);
}

/* Directory of the running executable, or "" if it cannot be found. */
static void app_dir(char *out, size_t len)
{
    image_info info;
    int32 cookie = 0;
    out[0] = '\0';
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
        if (info.type != B_APP_IMAGE)
            continue;
        snprintf(out, len, "%s", info.name);
        char *slash = strrchr(out, '/');
        if (slash)
            *slash = '\0';
        else
            out[0] = '\0';
        return;
    }
}

const JdBackend *audio_backend_load(const char *which, char *err, size_t err_len)
{
    return audio_backend_load_for(which, "JackDAW", "JACKDAW_BACKEND_DIR", err, err_len);
}

const JdBackend *audio_backend_load_for(const char *which, const char *app_name,
                                        const char *env_var, char *err, size_t err_len)
{
    if (which && strcmp(which, "mediakit") == 0) {
        const JdBackend *mk = jackdaw_mediakit_backend_entry(JD_BACKEND_ABI);
        if (!mk && err && err_len)
            snprintf(err, err_len, "The Media Kit backend does not match this %s.", app_name);
        return mk;
    }

    status_t why = B_ENTRY_NOT_FOUND;
    const JdBackend *b;
    char rest[B_FILE_NAME_LENGTH + 16];

    const char *env = env_var ? getenv(env_var) : NULL;
    if (env && *env && (b = try_path(env, "/" BACKEND_FILE, &why)) != NULL)
        return b;

    char dir[B_PATH_NAME_LENGTH];
    app_dir(dir, sizeof dir);
    if (dir[0] && (b = try_path(dir, "/backends/" BACKEND_FILE, &why)) != NULL)
        return b;

    static const directory_which dirs[] = {
        B_USER_NONPACKAGED_ADDONS_DIRECTORY,
        B_USER_ADDONS_DIRECTORY,
        B_SYSTEM_NONPACKAGED_ADDONS_DIRECTORY,
        B_SYSTEM_ADDONS_DIRECTORY,
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        if (find_directory(dirs[i], -1, false, dir, sizeof dir) != B_OK)
            continue;
        int n = snprintf(rest, sizeof rest, "/%s/" BACKEND_FILE, app_name);
        if (n < 0 || (size_t)n >= sizeof rest)
            break; /* a name that does not fit is not looked for */
        if ((b = try_path(dir, rest, &why)) != NULL)
            return b;
    }

    if (err && err_len) {
        if (why == B_ENTRY_NOT_FOUND)
            snprintf(err, err_len,
                     "JACK support is not installed: no %s/" BACKEND_FILE " add-on was found.",
                     app_name);
        else if (why == B_MISMATCHED_VALUES || why == B_BAD_VALUE)
            snprintf(err, err_len,
                     "The JACK support add-on (" BACKEND_FILE ") does not match this %s.",
                     app_name);
        else
            snprintf(err, err_len,
                     "The JACK support add-on (" BACKEND_FILE ") could not be loaded: %s.\n"
                     "Is the jack package installed?",
                     strerror(why));
    }
    return NULL;
}
