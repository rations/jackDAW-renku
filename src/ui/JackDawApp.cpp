#include "JackDawApp.h"

#include <glib.h>

#include <Messenger.h>

#include "engine/jackdaw-engine.h"
#include "host/pluginhost.h"
#include "MainWindow.h"
#include "ScanWindow.h"

JackDawApp::JackDawApp(JackDawProject *project)
    : BApplication("application/x-vnd.jackdaw"), m_project(project), m_main_window(NULL)
{
}

void JackDawApp::ReadyToRun()
{
    // Construct the window first so its engine-event hook is in place, then
    // bring the engine up, then show. All of this runs on the app thread; the
    // window's looper thread starts at Show().
    m_main_window = new MainWindow(m_project);
    if (jackdaw_engine_init(m_project))
        g_warning("JACK engine failed to start — running without audio");
    // Plugin host: RT process buffers are pre-allocated to the longest block
    // the engine can run (the period may change while running), so this must
    // come after the engine knows its period.
    pluginhost_init((double)jackdaw_engine_get_sample_rate(),
                    (int)jackdaw_engine_get_max_buffer_size());
    RunStartupScan();
    m_main_window->Show();
}

// Scan the plugin catalog before the main window is ever shown, so opening an
// FX window later never pays for it (matching the Linux original). The on-disk
// cache means only new or changed plug-ins are actually described, so this is
// near-instant on every run after the first.
//
// The scan runs here on the app thread; the progress window has its own looper
// and repaints itself, so it is only ever posted to, never locked.
void JackDawApp::RunStartupScan()
{
    ScanWindow *scan = new ScanWindow(pluginhost_scan_candidate_count());
    BMessenger scan_msgr(scan);
    scan->Show();

    pluginhost_set_scan_progress(&ScanWindow::ProgressHook, &scan_msgr);
    GList *added = pluginhost_scan_report_new();
    pluginhost_set_scan_progress(NULL, NULL);
    scan->PostMessage(B_QUIT_REQUESTED);

    if (!added)
        return;
    // Report what is new, and hold the main window back until it is dismissed.
    sem_id done = create_sem(0, "jackdaw_new_plugins");
    if (done >= 0) {
        (new NewPluginsWindow(added, done))->Show();
        while (acquire_sem(done) == B_INTERRUPTED) {
        }
        delete_sem(done);
    }
    g_list_free_full(added, g_free);
}

bool JackDawApp::QuitRequested()
{
    return BApplication::QuitRequested();
}
