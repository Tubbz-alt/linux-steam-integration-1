/*
 * This file is part of linux-steam-integration.
 *
 * Copyright © 2017 Ikey Doherty <ikey@solus-project.com>
 *
 * linux-steam-integration is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../common/files.h"
#include "../common/log.h"
#include "nica/util.h"

#include "redirect.h"

/**
 * Whether we've initialised yet or not.
 */
static volatile bool lsi_init = false;

/**
 * Whether we should actually perform overrides
 * This is determined by the process name
 */
static volatile bool lsi_override = false;

/**
 * Handle definitions
 */
typedef int (*lsi_open_file)(const char *p, int flags, mode_t mode);

/**
 * Global storage of handles for nicer organisation.
 */
typedef struct LsiRedirectTable {
        lsi_open_file open;

        /* Allow future handle opens.. */
        struct {
                void *libc;
        } handles;
} LsiRedirectTable;

/**
 * Our redirect instance, stack only.
 */
static LsiRedirectTable lsi_table = { 0 };

/**
 * Contains all of our replacement rules.
 */
static LsiRedirectProfile *lsi_profile = NULL;

/**
 * We'll only perform teardown code if the process doesn't `abort()` or `_exit()`
 */
__attribute__((destructor)) static void lsi_redirect_shutdown(void)
{
        if (lsi_profile) {
                lsi_redirect_profile_free(lsi_profile);
                lsi_profile = NULL;
        }

        if (!lsi_init) {
                return;
        }
        lsi_init = false;

        if (lsi_table.handles.libc) {
                dlclose(lsi_table.handles.libc);
                lsi_table.handles.libc = NULL;
        }
}

/**
 * Responsible for setting up the vfunc redirect table so that we're able to
 * get `open` and such working before we've hit the constructor, and ensure
 * we only init once.
 */
static void lsi_redirect_init_tables(void)
{
        void *symbol_lookup = NULL;
        char *dl_error = NULL;

        if (lsi_init) {
                return;
        }

        lsi_init = true;

        /* Try to explicitly open libc. We can't safely rely on RTLD_NEXT
         * as we might be dealing with a strange link order
         */
        lsi_table.handles.libc = dlopen("libc.so.6", RTLD_LAZY);
        if (!lsi_table.handles.libc) {
                fprintf(stderr, "Unable to grab libc.so.6 handle: %s\n", dlerror());
                goto failed;
        }

        /* Safely look up the symbol */
        symbol_lookup = dlsym(lsi_table.handles.libc, "open");
        dl_error = dlerror();
        if (dl_error || !symbol_lookup) {
                fprintf(stderr, "Failed to bind 'open': %s\n", dl_error);
                goto failed;
        }

        /* Have the symbol correctly, copy it to make it usable */
        memcpy(&lsi_table.open, &symbol_lookup, sizeof(lsi_table.open));

        /* We might not get an unload, so chain onto atexit */
        atexit(lsi_redirect_shutdown);
        return;

failed:
        /* Pretty damn fatal. */
        lsi_redirect_shutdown();
        abort();
}

/**
 * Testing: Create profile for ARK: Survival Evolved
 */
static LsiRedirectProfile *lsi_redirect_profile_new_ark(void)
{
        LsiRedirectProfile *p = NULL;
        LsiRedirect *redirect = NULL;

        autofree(char) *steam_dir = NULL;
        autofree(char) *mic_source = NULL;
        autofree(char) *mic_target = NULL;
        static const char *def_mic_source =
            "steamapps/common/ARK/ShooterGame/Content/PrimalEarth/Environment/Water/"
            "Water_DepthBlur_MIC.uasset";
        static const char *def_mic_target =
            "steamapps/common/ARK/ShooterGame/Content/Mods/TheCenter/Assets/Mic/"
            "Water_DepthBlur_MIC.uasset";

        p = lsi_redirect_profile_new("ARK: Survival Evolved");
        if (!p) {
                fputs("OUT OF MEMORY\n", stderr);
                return NULL;
        }

        steam_dir = lsi_get_steam_dir();
        if (asprintf(&mic_source, "%s/%s", steam_dir, def_mic_source) < 0) {
                goto failed;
        }
        if (asprintf(&mic_target, "%s/%s", steam_dir, def_mic_target) < 0) {
                goto failed;
        }

        redirect = lsi_redirect_new_path_replacement(mic_source, mic_target);
        if (!redirect) {
                goto failed;
        }
        lsi_redirect_profile_insert_rule(p, redirect);
        return p;

failed:
        /* Dead in the water.. */
        fputs("OUT OF MEMORY\n", stderr);
        lsi_redirect_profile_free(p);
        return NULL;
}

/**
 * Main entry point into this redirect module.
 *
 * We'll check the process name and determine if we're interested in installing
 * some redirect hooks into this library.
 * Otherwise we'll continue to operate in a pass-through mode
 */
__attribute__((constructor)) static void lsi_redirect_init(void)
{
        /* Ensure we're open. */
        lsi_redirect_init_tables();
        autofree(char) *process_name = NULL;

        process_name = lsi_get_process_name();

        if (!process_name) {
                fputs("Out of memory!\n", stderr);
                return;
        }

        /* Temporary hack, we'll make this more generic later */
        if (strcmp(process_name, "ShooterGame") == 0) {
                lsi_log_set_id("ShooterGame");
                lsi_profile = lsi_redirect_profile_new_ark();
        }

        if (!lsi_profile) {
                return;
        }

        lsi_override = true;

        lsi_log_debug("Enable lsi_redirect for '%s'", lsi_profile->name);
}

_nica_public_ int open(const char *p, int flags, ...)
{
        va_list va;
        mode_t mode;
        autofree(char) *replaced_path = NULL;
        LsiRedirect *redirect = NULL;
        LsiRedirectOperation op = LSI_OPERATION_OPEN;
        autofree(char) *path = NULL;

        /* Must ensure we're **really** initialised, as we might see open happen
         * before the constructor..
         */
        lsi_redirect_init_tables();

        /* Grab the mode_t */
        va_start(va, flags);
        mode = va_arg(va, mode_t);
        va_end(va);

        /* Not interested in this guy apparently */
        if (!lsi_override) {
                return lsi_table.open(p, flags, mode);
        }

        /* Get the absolute path here */
        path = realpath(p, NULL);
        if (!path) {
                return lsi_table.open(p, flags, mode);
        }

        /* find a valid replacement */
        for (redirect = lsi_profile->op_table[op]; redirect; redirect = redirect->next) {
                /* Currently we only known path replacements */
                if (redirect->type != LSI_REDIRECT_PATH) {
                        continue;
                }
                if (strcmp(redirect->path_source, path) != 0) {
                        continue;
                }
                if (!lsi_file_exists(redirect->path_target)) {
                        lsi_log_warn("Replacement path does not exist: %s", redirect->path_target);
                }
                lsi_log_info("Replaced '%s' with '%s'", path, redirect->path_target);
                return lsi_table.open(redirect->path_target, flags, mode);
        }

        /* No replacement */
        return lsi_table.open(p, flags, mode);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 expandtab:
 * :indentSize=8:tabSize=8:noTabs=true:
 */