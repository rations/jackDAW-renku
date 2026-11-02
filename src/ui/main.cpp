#include <cstdlib>

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

    {
        JackDawApp app(project);
        app.Run();
        // Close the audio backend while the application object still exists:
        // ~BApplication quits the loopers registered with it, BMediaRoster
        // among them (MediaRoster.cpp registers it with be_app), and the
        // Media Kit backend needs the roster to stop its node and undo its
        // connections.
        jackdaw_engine_quit();
    }

    g_object_unref(project);
    pluginhost_shutdown();
    settings_quit();

    return EXIT_SUCCESS;
}
