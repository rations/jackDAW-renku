#pragma once

#include <String.h>
#include <Window.h>

#include <vector>

#include "engine/project.h"
#include "engine/render.h"

class BFilePanel;
class BLayoutItem;
class BMenuBar;
class BMessageRunner;
class BPoint;
class CountInWindow;
class FxWindow;
class IoWindow;
class MetroVolumeWindow;
class MidiControlWindow;
class MidiWindow;
class MixerView;
class MixerWindow;
class RenderWindow;
class TimelineView;
class TransportView;

class MainWindow : public BWindow
{
public:
    explicit MainWindow(JackDawProject *project);
    ~MainWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    // Rebuild both mixers' channel strips (called from GObject track-list
    // signal trampolines, which run on this looper).
    void RebuildMixers();

    // Piano-roll editor registry. Editors acquire this window's looper lock
    // for model access, so this window must never lock them — communication
    // back is async PostMessage only. Unregister runs on the editor's thread
    // (its destructor) while holding this window's lock.
    void UnregisterMidiEditor(MidiWindow *w);
    void UnregisterFxWindow(FxWindow *w);

    // Invoked by the midicontrol dispatch hooks (which run on this looper via the
    // control-poll tick): a mapped transport action, and the post-learn refresh.
    void MidiCtlTransport(int which);
    void SendMidiCtlSnapshot();

private:
    BMenuBar *BuildMenuBar();

    void TransportPlay();
    void TransportStop();
    void TransportRecord();
    void TransportToggle();
    void TransportPause();
    void StepCursor(int direction); // -1 / +1: nudge the cursor by 10 ms
    void LocateNextBoundary();      // ▶| — next clip boundary (loop edges until P6)

    // Track management (single-mutator: all engine/project edits run here).
    void AddTrack(JackDawTrackKind kind);
    void DeleteTrack(JackDawTrack *track);
    void ShowTrackContext(int slot, BPoint screen_where);
    // Right-click on the empty strip column: Add Track / Add MIDI Track.
    void ShowTrackAreaContext(BPoint screen_where);
    // Import an audio file onto a fresh audio track (region placed at timeline 0).
    void LoadFileAsTrack(const char *path);

    // Project save/load (single-mutator: all engine/project edits run here).
    bool SaveProjectTo(const char *path);   // write bundle, title, last-project; true = saved
    void ShowSaveAsPanel();                 // save panel seeded with the current name
    void ShowOpenPanel();                   // open panel in the projects dir
    void OpenProject(const char *path);     // stop, tear down, load, rebuild UI
    void NewSession();                      // clear all tracks; reset to defaults
    void UpdateTitle();                     // window title from the project file name
    void CloseAllMidiEditors();             // async-quit every piano-roll + drain
    void OpenFxEditor(JackDawTrack *track); // one FX window per track (raise if open)
    void CloseAllFxWindows();               // async-quit every FX window + drain
    // Ask "Save changes before ...?" before something replaces the session
    // (quit, New Session, Open Project). The project keeps no unsaved-changes
    // flag, so it always asks. The action runs on No, or on Yes once the save
    // succeeded; Cancel, Escape, a cancelled Save As or a failed save keep the
    // session. Asynchronous: BAlert and the save panel reply by message.
    void ConfirmSaveThen(int action, const char *question);
    void RunPendingAction();

    // Render/export dialog (P11); region=TRUE renders the loop-tab selection.
    void OpenRenderDialog(bool region);
    void StartRenderFromMessage(BMessage *msg); // build options + kick off
    void RenderTick();                          // poll progress; finalize when done
    void SendRenderProgress(int state);         // notify the dialog (0..3)
    void CleanupRender();                       // join/stop, drop tick + thread

    // Right-click context popups (built fresh so their marks are current).
    void ShowRecordMenu(BPoint screen_where);
    void ShowMetroMenu(BPoint screen_where);
    void ShowMixerMenu(BPoint screen_where);

    // Metronome option windows (lazily created singletons, hidden not destroyed).
    void OpenMetroVolumeWindow();
    void OpenCountInWindow();

    // Options -> Inputs/Outputs (lazy singleton; re-synced to live port counts).
    void OpenIoWindow();

    // Options -> MIDI Control surface. The mapping table + engine control_in are
    // owned here (single mutator); the dialog is a separate-looper view that
    // posts edits and receives snapshots. The poll tick drains control_in and
    // dispatches mappings regardless of whether the dialog is open.
    void OpenMidiControlWindow();
    void MidiControlPoll(); // drain control_in -> midicontrol_dispatch_event
    // (MidiCtlTransport / SendMidiCtlSnapshot are public: called from the
    //  file-static midicontrol callback trampolines.)

    // Piano-roll MIDI editor (per-instrument-track window; presents if open).
    void OpenMidiEditor(JackDawTrack *track);
    bool TrackInProject(JackDawTrack *track) const;

    // Mixer (docked pane + optional detached window).
    void ApplyMixerState();               // show/hide dock vs. window
    void SyncMixers();                    // per-tick control/meter refresh
    JackDawTrack *TrackForSlot(int slot); // map a mixer/strip slot to its track

    JackDawProject *m_project; // borrowed; main() owns the reference

    TransportView *m_transport;
    TimelineView *m_timeline;
    BMessageRunner *m_tick_runner;

    MetroVolumeWindow *m_metro_volume_window; // NULL until first opened
    CountInWindow *m_countin_window;          // NULL until first opened
    IoWindow *m_io_window;                    // NULL until first opened
    MidiControlWindow *m_midictl_window;      // NULL until first opened
    BMessageRunner *m_midictl_tick;           // control_in poll timer

    MixerView *m_mixer;          // docked pane
    BLayoutItem *m_mixer_item;   // its layout slot (for collapse)
    MixerWindow *m_mixer_window; // detached window (NULL until first detach)
    bool m_mixer_visible;        // user toggled the mixer on
    float m_dock_h_applied;      // window height added for the docked mixer (0 = none)

    BFilePanel *m_load_panel; // "Load File as New Track" open panel (lazy)
    BFilePanel *m_save_panel; // "Save Project As" save panel (lazy)
    BFilePanel *m_open_panel; // "Open Project" open panel (lazy)

    // Save-before-replacing prompt state (see ConfirmSaveThen).
    enum { PENDING_NONE = 0, PENDING_QUIT, PENDING_NEW, PENDING_OPEN };
    int m_pending_action;        // waiting on the prompt or its Save As
    bool m_pending_save_as;      // the Save As panel is open for m_pending_action
    bool m_quit_confirmed;       // QuitRequested may go ahead without asking
    BString m_pending_open_path; // PENDING_OPEN: the project chosen

    RenderWindow *m_render_window; // render/export dialog (NULL until opened)

    // Active render state (driven on this looper; see the render dialog note).
    JackDawRenderProgress m_render_prog;
    GThread *m_render_thread;      // offline worker (NULL for realtime/idle)
    BMessageRunner *m_render_tick; // poll timer while a render runs
    RenderMethod m_render_method;
    bool m_render_active;

    // Open piano-roll editors (guarded by this window's looper lock).
    std::vector<MidiWindow *> m_midi_editors;
    std::vector<FxWindow *> m_fx_windows;

    gulong m_track_added_h;
    gulong m_track_removed_h;
    gulong m_tracks_reordered_h;
};
