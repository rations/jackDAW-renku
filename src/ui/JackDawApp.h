#pragma once

#include <Application.h>
#include <String.h>

#include "engine/project.h"

class MainWindow;

class JackDawApp : public BApplication
{
public:
    explicit JackDawApp(JackDawProject *project);

    void ArgvReceived(int32 argc, char **argv) override;
    void ReadyToRun() override;
    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    // After Run() returns: whether the main window asked for a restart (a new
    // audio system was chosen), and the project to reopen ("" for none).
    bool RestartRequested() const { return m_restart; }
    const char *RestartProject() const { return m_restart_path.String(); }

private:
    // Catalog scan behind a progress window, before the main window is shown.
    void RunStartupScan();

    JackDawProject *m_project; // borrowed; main() owns the reference
    MainWindow *m_main_window;
    BString m_open_path; // project named on the command line, until ReadyToRun
    bool m_restart;
    BString m_restart_path;
};
