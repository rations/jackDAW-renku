#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <Roster.h>
#include <String.h>

#include <glib-object.h>

#include "engine/glib_check.h"
#include "engine/jackdaw-engine.h"
#include "engine/project.h"
#include "engine/settings.h"
#include "host/pluginhost.h"
#include "JackDawApp.h"

int main(int argc, char **argv)
{
    // `JackDAW --scan-plugin VST3 <path>`: throwaway plugin-describe process
    // spawned by the catalog scan. Runs before any app/engine object so a
    // crashing third-party module can only take this helper down.
    int scan_rc = pluginhost_scan_helper_main(argc, argv);
    if (scan_rc >= 0)
        return scan_rc;

    // Verify the glib2 runtime (GObject + atomics) before any engine or UI
    // object exists; the engine/model layer is built on it.
    if (jackdaw_glib_check() != 0)
        return EXIT_FAILURE;

    settings_init();

    JackDawProject *project = jackdaw_project_new();

    // A restart (Options -> Audio System) starts this executable again once
    // everything here is shut down: the audio backend closed (the Media Kit
    // node gone, the JACK client closed) and the application unregistered.
    bool restart = false;
    BString restart_project;
    entry_ref app_ref;
    bool have_app_ref = false;
    {
        JackDawApp app(project);
        app_info info;
        if (app.GetAppInfo(&info) == B_OK) {
            app_ref = info.ref;
            have_app_ref = true;
        }
        app.Run();
        // Close the audio backend while the application object still exists:
        // ~BApplication quits the loopers registered with it, BMediaRoster
        // among them (MediaRoster.cpp registers it with be_app), and the
        // Media Kit backend needs the roster to stop its node and undo its
        // connections.
        jackdaw_engine_quit();
        restart = app.RestartRequested();
        restart_project = app.RestartProject();
    }

    g_object_unref(project);
    pluginhost_shutdown();
    settings_quit();

    if (restart && have_app_ref) {
        // The new instance would otherwise inherit an override of the choice
        // just made (BRoster::Launch passes this team's environment on).
        unsetenv("JACKDAW_BACKEND");
        const char *args[1] = {restart_project.String()};
        int nargs = restart_project.Length() > 0 ? 1 : 0;
        status_t err = be_roster->Launch(&app_ref, nargs, args);
        if (err != B_OK)
            fprintf(stderr, "JackDAW: could not start again: %s\n", strerror(err));
    }

    return EXIT_SUCCESS;
}
