#include "JackDawApp.h"

#include <glib.h>

#include <Alert.h>
#include <Messenger.h>

#include "engine/jackdaw-engine.h"
#include "host/pluginhost.h"
#include "MainWindow.h"
#include "Messages.h"
#include "ScanWindow.h"

JackDawApp::JackDawApp(JackDawProject *project)
    : BApplication("application/x-vnd.jackdaw"), m_project(project), m_main_window(NULL),
      m_restart(false)
{
}

// `JackDAW <project.jdaw>`: open that project once the window is up. This is
// how a restart onto another audio system brings the session back. Delivered
// before ReadyToRun at launch (BApplication posts B_ARGV_RECEIVED first).
void JackDawApp::ArgvReceived(int32 argc, char **argv)
{
    if (argc < 2 || !argv[1] || !argv[1][0])
        return;
    if (m_main_window == NULL) {
        m_open_path = argv[1];
        return;
    }
    // Already running: ask about unsaved changes like File -> Open does.
    BMessage open(MSG_OPEN_PROJECT_PATH);
    open.AddString("path", argv[1]);
    open.AddBool("ask", true);
    m_main_window->PostMessage(&open);
}

void JackDawApp::ReadyToRun()
{
    // Construct the window first so its engine-event hook is in place, then
    // bring the engine up, then show. All of this runs on the app thread; the
    // window's looper thread starts at Show().
    m_main_window = new MainWindow(m_project);
    if (jackdaw_engine_init(m_project))
        g_warning("audio engine failed to start — running without audio");
    // Plugin host: RT process buffers are pre-allocated to the longest block
    // the engine can run (the period may change while running), so this must
    // come after the engine knows its period.
    pluginhost_init((double)jackdaw_engine_get_sample_rate(),
                    (int)jackdaw_engine_get_max_buffer_size());
    RunStartupScan();
    m_main_window->Show();

    if (m_open_path.Length() > 0) {
        BMessage open(MSG_OPEN_PROJECT_PATH);
        open.AddString("path", m_open_path.String());
        m_main_window->PostMessage(&open);
        m_open_path = "";
    }

    // JACK chosen but not running (so the Media Kit is used), or no audio at
    // all: say so. Go(NULL) runs the alert asynchronously.
    const char *notice = jackdaw_engine_get_backend_notice();
    if (notice) {
        BAlert *alert = new BAlert("Audio system", notice, "OK", NULL, NULL, B_WIDTH_AS_USUAL,
                                   jackdaw_engine_is_running() ? B_INFO_ALERT : B_WARNING_ALERT);
        alert->Go(NULL);
    }
}

void JackDawApp::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_APP_RESTART:
            m_restart = true;
            m_restart_path = message->GetString("path", "");
            break;
        default:
            BApplication::MessageReceived(message);
            break;
    }
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
