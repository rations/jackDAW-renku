#include "MainWindow.h"

#include <Alert.h>
#include <Application.h>
#include <Entry.h>
#include <FilePanel.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <LayoutItem.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <String.h>
#include <TextView.h>

#include <stdio.h>
#include <stdlib.h>

#include "engine/jackdaw-engine.h"
#include "engine/midicontrol.h"
#include "engine/settings.h"
#include "host/pluginhost.h"
#include "Messages.h"
#include "FxWindow.h"
#include "IoWindow.h"
#include "MetronomeWindows.h"
#include "MidiControlWindow.h"
#include "MidiWindow.h"
#include "MixerView.h"
#include "MixerWindow.h"
#include "RenderWindow.h"
#include "ScanWindow.h"
#include "TimelineView.h"
#include "TrackAreaView.h"
#include "TransportView.h"

// GObject "track-added/removed/reordered" -> refresh both mixers. Emitted only
// on this (the MainWindow) looper, so touching the docked mixer is safe; the
// detached window is refreshed via a posted message on its own looper.
static void mainwin_tracks_changed_2(gpointer, gpointer, gpointer user)
{
    static_cast<MainWindow *>(user)->RebuildMixers();
}
static void mainwin_tracks_reordered(gpointer, gpointer user)
{
    static_cast<MainWindow *>(user)->RebuildMixers();
}

// UI refresh cadence (playhead readout, ruler/lane playhead redraws).
static const bigtime_t kTickIntervalUsec = 33000; // ~30 Hz

// Control-surface poll cadence: drains control_in and dispatches mappings. Kept
// snappy so a footswitch feels responsive; the work is tiny (a ring drain).
static const bigtime_t kMidiCtlPollUsec = 20000; // ~50 Hz

// midicontrol transport/changed hooks run on the MainWindow looper (the poll
// tick calls dispatch there), so they can touch this window directly.
static void midictl_transport_cb(int which, void *user)
{
    static_cast<MainWindow *>(user)->MidiCtlTransport(which);
}
static void midictl_changed_cb(void *user)
{
    static_cast<MainWindow *>(user)->SendMidiCtlSnapshot();
}
// A mapping changed an effect's bypass or wet/dry on `t`: let its open FX
// window show it. Runs on this window's thread (the MIDI-control poll), the
// thread that owns the track's "fx-window" pointer; the FX window refreshes on
// its own looper.
static void midictl_fx_cb(JackDawTrack *t, void *user)
{
    (void)user;
    if (!t)
        return;
    FxWindow *fw = (FxWindow *)g_object_get_data(G_OBJECT(t), "fx-window");
    if (fw)
        fw->PostMessage('fxsy'); // MSG_FX_SYNC
}

// Fixed height of the docked mixer pane. Showing it grows the window by this
// much so the mixer docks under the timeline without shrinking it.
static const float kDockedMixerH = 250.0f;

// Target for engine events. Written only from the creating thread while the
// engine hook is unset (constructor / QuitRequested), read by JACK
// notification threads through the hook — so there is never a concurrent
// write. The object itself has process lifetime; a send to a dead window
// fails harmlessly.
static BMessenger g_engine_messenger;

// Runs on JACK notification threads: non-blocking post only (timeout 0);
// dropping the message is fine, handlers re-read engine state.
static void engine_event_hook(int event, void *user)
{
    (void)user;
    uint32 what;
    switch (event) {
        case JACKDAW_ENGINE_EVENT_PORTS_CHANGED:
            what = MSG_ENGINE_PORTS_CHANGED;
            break;
        case JACKDAW_ENGINE_EVENT_CONNECTIONS_CHANGED:
            what = MSG_ENGINE_CONNECTIONS_CHANGED;
            break;
        case JACKDAW_ENGINE_EVENT_SHUTDOWN:
            what = MSG_ENGINE_SHUTDOWN;
            break;
        case JACKDAW_ENGINE_EVENT_TAKE_FINALIZED:
            what = MSG_ENGINE_TAKE_READY;
            break;
        case JACKDAW_ENGINE_EVENT_MIDI_TAKE_FINALIZED:
            what = MSG_ENGINE_MIDI_TAKE_READY;
            break;
        default:
            return;
    }
    BMessage msg(what);
    g_engine_messenger.SendMessage(&msg, (BHandler *)NULL, 0);
}

// Window-level transport keys. Swallows the plain key so a focused BButton
// doesn't also react — but leaves everything alone while the user is typing in
// a text control (track names, BPM).
class TransportKeyFilter : public BMessageFilter
{
public:
    TransportKeyFilter() : BMessageFilter(B_KEY_DOWN)
    {
    }

    filter_result Filter(BMessage *message, BHandler **target) override
    {
        (void)target;
        BWindow *window = static_cast<BWindow *>(Looper());
        if (dynamic_cast<BTextView *>(window->CurrentFocus()) != NULL)
            return B_DISPATCH_MESSAGE; /* typing in a text field */

        // Ctrl+<letter> edit shortcuts. Haiku only resolves menu/window
        // shortcuts while the Command key (Alt on the default keymap) is held,
        // so Ctrl combos must be caught here. raw_char is the base letter with
        // no modifiers applied (byte would be the control code under Ctrl).
        int32 mods = 0, raw = 0;
        message->FindInt32("modifiers", &mods);
        message->FindInt32("raw_char", &raw);
        if (mods & B_CONTROL_KEY) {
            uint32 what = 0;
            switch (raw) {
                case 'z':
                    what = (mods & B_SHIFT_KEY) ? MSG_EDIT_REDO : MSG_EDIT_UNDO;
                    break;
                case 'y':
                    what = MSG_EDIT_REDO;
                    break;
                case 'c':
                    what = MSG_REGION_COPY;
                    break;
                case 'v':
                    what = MSG_REGION_PASTE;
                    break;
                case 'g':
                    what = MSG_REGION_GROUP;
                    break;
            }
            if (what) {
                window->PostMessage(what);
                return B_SKIP_MESSAGE;
            }
            return B_DISPATCH_MESSAGE;
        }

        int8 byte;
        if (message->FindInt8("byte", &byte) != B_OK)
            return B_DISPATCH_MESSAGE;

        switch (byte) {
            case B_SPACE:
                window->PostMessage(MSG_TRANSPORT_TOGGLE);
                return B_SKIP_MESSAGE;
            case B_HOME:
                window->PostMessage(MSG_TRANSPORT_RTZ);
                return B_SKIP_MESSAGE;
            case B_LEFT_ARROW:
                window->PostMessage(MSG_TRANSPORT_STEP_BACK);
                return B_SKIP_MESSAGE;
            case B_RIGHT_ARROW:
                window->PostMessage(MSG_TRANSPORT_STEP_FWD);
                return B_SKIP_MESSAGE;
            case 'r':
            case 'R':
                window->PostMessage(MSG_TRANSPORT_RECORD);
                return B_SKIP_MESSAGE;
            case 'l':
            case 'L':
                window->PostMessage(MSG_TRANSPORT_LOOP);
                return B_SKIP_MESSAGE;
            case 's':
            case 'S':
                window->PostMessage(MSG_SPLIT);
                return B_SKIP_MESSAGE;
            case B_DELETE:
            case B_BACKSPACE:
                window->PostMessage(MSG_REGION_DELETE);
                return B_SKIP_MESSAGE;
            default:
                return B_DISPATCH_MESSAGE;
        }
    }
};

// Clicking anywhere that is not the focused text field releases that field's
// focus and selection — window-wide, so a stuck BPM/timesig entry can't
// swallow the transport keys.
class FocusReleaseFilter : public BMessageFilter
{
public:
    FocusReleaseFilter() : BMessageFilter(B_MOUSE_DOWN)
    {
    }

    filter_result Filter(BMessage *message, BHandler **target) override
    {
        (void)message;
        BWindow *window = static_cast<BWindow *>(Looper());
        BTextView *focus = dynamic_cast<BTextView *>(window->CurrentFocus());
        if (focus != NULL && *target != focus) {
            focus->MakeFocus(false);
            focus->Select(0, 0);
        }
        return B_DISPATCH_MESSAGE;
    }
};

// Add a menu item, optionally disabled until its feature phase lands.
static BMenuItem *AddItem(BMenu *menu, const char *label, uint32 what, char shortcut = 0,
                          uint32 mods = 0, bool enabled = true)
{
    BMenuItem *item = new BMenuItem(label, new BMessage(what), shortcut, mods);
    item->SetEnabled(enabled);
    menu->AddItem(item);
    return item;
}

MainWindow::MainWindow(JackDawProject *project)
    : BWindow(BRect(100, 100, 1100, 700), "JackDAW", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
      m_project(project), m_transport(NULL), m_timeline(NULL), m_tick_runner(NULL),
      m_metro_volume_window(NULL), m_countin_window(NULL), m_io_window(NULL),
      m_midictl_window(NULL), m_midictl_tick(NULL), m_mixer(NULL), m_mixer_item(NULL),
      m_mixer_window(NULL), m_mixer_visible(false), m_dock_h_applied(0.0f), m_load_panel(NULL),
      m_save_panel(NULL), m_open_panel(NULL), m_render_window(NULL), m_render_thread(NULL),
      m_render_tick(NULL), m_render_method(RENDER_METHOD_OFFLINE), m_render_active(false),
      m_track_added_h(0), m_track_removed_h(0), m_tracks_reordered_h(0)
{
    memset(&m_render_prog, 0, sizeof(m_render_prog));
    // Restore the saved window frame (defaults match the BWindow rect above).
    {
        float x = (float)(gint32)settings_get_uint32("win_x", 100);
        float y = (float)(gint32)settings_get_uint32("win_y", 100);
        float w = (float)settings_get_uint32("win_w", 1000);
        float h = (float)settings_get_uint32("win_h", 600);
        if (w >= 400 && h >= 300 && w < 20000 && h < 20000) {
            MoveTo(x, y);
            ResizeTo(w, h);
        }
    }
    BMenuBar *menu_bar = BuildMenuBar();
    m_transport = new TransportView(project);
    m_timeline = new TimelineView(project);
    m_mixer = new MixerView(project, BMessenger(this));
    // The docked mixer is a fixed-height pane: showing it grows the window by
    // exactly this height (docking under the timeline without shrinking it),
    // hiding it shrinks the window back. See ApplyMixerState.
    m_mixer->SetExplicitMinSize(BSize(B_SIZE_UNSET, kDockedMixerH));
    m_mixer->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, kDockedMixerH));

    // No JACK status bar: server health/xrun monitoring is the patchbay's job
    // (JackGraph), matching the Linux JackDAW main window.
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(menu_bar).Add(m_transport).Add(m_timeline);
    m_mixer_item = GetLayout()->AddView(m_mixer); // docked pane below the timeline
    m_mixer_item->SetVisible(false);              // hidden until toggled on

    // Track-list changes rebuild the mixer channel strips.
    m_track_added_h =
        g_signal_connect(project, "track-added", G_CALLBACK(mainwin_tracks_changed_2), this);
    m_track_removed_h =
        g_signal_connect(project, "track-removed", G_CALLBACK(mainwin_tracks_changed_2), this);
    m_tracks_reordered_h =
        g_signal_connect(project, "tracks-reordered", G_CALLBACK(mainwin_tracks_reordered), this);

    AddCommonFilter(new TransportKeyFilter());
    AddCommonFilter(new FocusReleaseFilter());

    // Diagnostic switch: JACKDAW_NO_TICK=1 disables the periodic UI refresh,
    // to isolate whether the 30 Hz redraw traffic is implicated in xruns.
    if (getenv("JACKDAW_NO_TICK") == NULL)
        m_tick_runner =
            new BMessageRunner(BMessenger(this), BMessage(MSG_UI_TICK), kTickIntervalUsec);

    // Register for engine events before the engine is initialised so none are
    // missed (both happen on the creating thread, pre-Show).
    g_engine_messenger = BMessenger(this);
    jackdaw_engine_set_event_hook(engine_event_hook, NULL);

    // MIDI control surface: the mapping table lives here (single mutator). Route
    // transport actions through this window and refresh the dialog after a learn.
    midicontrol_init();
    midicontrol_set_transport_cb(midictl_transport_cb, this);
    midicontrol_set_changed_cb(midictl_changed_cb, this);
    midicontrol_set_fx_cb(midictl_fx_cb, this);
    m_midictl_tick =
        new BMessageRunner(BMessenger(this), BMessage(MSG_MIDICTL_POLL), kMidiCtlPollUsec);
}

MainWindow::~MainWindow()
{
    if (m_track_added_h)
        g_signal_handler_disconnect(m_project, m_track_added_h);
    if (m_track_removed_h)
        g_signal_handler_disconnect(m_project, m_track_removed_h);
    if (m_tracks_reordered_h)
        g_signal_handler_disconnect(m_project, m_tracks_reordered_h);
    delete m_tick_runner;
    delete m_midictl_tick;
    midicontrol_shutdown();
    delete m_load_panel;
    delete m_save_panel;
    delete m_open_panel;
}

void MainWindow::ApplyMixerState()
{
    bool in_window = settings_get_uint32("mixer_in_window", 0) != 0;
    bool want_dock = m_mixer_visible && !in_window;
    bool want_win = m_mixer_visible && in_window;

    if (m_mixer_item) {
        m_mixer_item->SetVisible(want_dock);
        // Dock the fixed-height mixer *under* the timeline without shrinking it:
        // grow the window by the pane height when showing, shrink it back when
        // hiding. Tracked so repeated ApplyMixerState calls (e.g. dock<->window
        // switches) never double-count the delta.
        float target = want_dock ? kDockedMixerH : 0.0f;
        float delta = target - m_dock_h_applied;
        if (delta != 0.0f) {
            ResizeBy(0.0f, delta);
            m_dock_h_applied = target;
        }
    }

    if (want_win) {
        if (m_mixer_window == NULL)
            m_mixer_window = new MixerWindow(m_project, BMessenger(this));
        if (m_mixer_window->LockLooper()) {
            m_mixer_window->Rebuild();
            if (m_mixer_window->IsHidden())
                m_mixer_window->Show();
            m_mixer_window->Activate();
            m_mixer_window->UnlockLooper();
        }
    } else if (m_mixer_window && m_mixer_window->LockLooper()) {
        if (!m_mixer_window->IsHidden())
            m_mixer_window->Hide();
        m_mixer_window->UnlockLooper();
    }
}

void MainWindow::RebuildMixers()
{
    if (m_mixer)
        m_mixer->Rebuild();
    if (m_mixer_window)
        BMessenger(m_mixer_window).SendMessage(MSG_MIXER_REBUILD);
}

void MainWindow::SyncMixers()
{
    bool in_window = settings_get_uint32("mixer_in_window", 0) != 0;
    if (m_mixer && m_mixer_visible && !in_window) {
        m_mixer->Sync();
        m_mixer->UpdateMeters();
    }
    // The detached window refreshes itself on its own runner tick.
}

JackDawTrack *MainWindow::TrackForSlot(int slot)
{
    if (slot < 0)
        return NULL; // master
    guint n = jackdaw_project_track_count(m_project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *t = jackdaw_project_get_track(m_project, i);
        if ((int)t->slot == slot)
            return t;
    }
    return NULL;
}

BMenuBar *MainWindow::BuildMenuBar()
{
    BMenuBar *bar = new BMenuBar("menubar");

    // File — session I/O (save/load) and render/export.
    BMenu *file = new BMenu("File");
    AddItem(file, "Open Project…", MSG_FILE_OPEN, 'O', B_COMMAND_KEY);
    AddItem(file, "Save Project", MSG_FILE_SAVE, 'S', B_COMMAND_KEY);
    AddItem(file, "Save Project As…", MSG_FILE_SAVE_AS, 'S', B_COMMAND_KEY | B_SHIFT_KEY);
    file->AddSeparatorItem();
    AddItem(file, "Render…", MSG_FILE_RENDER);
    AddItem(file, "Render Region…", MSG_FILE_RENDER_REGION);
    file->AddSeparatorItem();
    AddItem(file, "New Session", MSG_FILE_NEW, 'N', B_COMMAND_KEY);
    file->AddSeparatorItem();
    AddItem(file, "Quit", B_QUIT_REQUESTED, 'Q', B_COMMAND_KEY);
    bar->AddItem(file);

    // Edit — undo/redo + region clipboard/editing (act on the active track's
    // section selection, else the rubber-band range).
    BMenu *edit = new BMenu("Edit");
    AddItem(edit, "Undo", MSG_EDIT_UNDO, 'Z', B_COMMAND_KEY);
    AddItem(edit, "Redo", MSG_EDIT_REDO, 'Y', B_COMMAND_KEY);
    edit->AddSeparatorItem();
    AddItem(edit, "Copy", MSG_REGION_COPY, 'C', B_COMMAND_KEY);
    AddItem(edit, "Paste", MSG_REGION_PASTE, 'V', B_COMMAND_KEY);
    AddItem(edit, "Delete", MSG_REGION_DELETE);
    AddItem(edit, "Group Sections", MSG_REGION_GROUP, 'G', B_COMMAND_KEY);
    edit->AddSeparatorItem();
    AddItem(edit, "Split at Playhead", MSG_SPLIT);
    bar->AddItem(edit);

    // Track — add/delete are live now; Load File arrives with the clip phase.
    BMenu *track = new BMenu("Track");
    AddItem(track, "Add Empty Track", MSG_TRACK_ADD, 'T', B_COMMAND_KEY);
    AddItem(track, "Add MIDI Track", MSG_TRACK_ADD_MIDI, 'T', B_COMMAND_KEY | B_SHIFT_KEY);
    AddItem(track, "Load File as New Track…", MSG_TRACK_LOAD_FILE);
    track->AddSeparatorItem();
    AddItem(track, "Delete Active Track", MSG_TRACK_DELETE, 0, 0);
    bar->AddItem(track);

    // Transport — all functional now.
    BMenu *transport = new BMenu("Transport");
    AddItem(transport, "Play / Stop", MSG_TRANSPORT_TOGGLE);
    AddItem(transport, "Record", MSG_TRANSPORT_RECORD);
    AddItem(transport, "Loop", MSG_TRANSPORT_LOOP);
    transport->AddSeparatorItem();
    AddItem(transport, "Locate to Start", MSG_TRANSPORT_RTZ);
    bar->AddItem(transport);

    // View — zoom + metronome options now; the mixer-in-window setting is
    // stored now and honoured once the mixer view exists.
    BMenu *view = new BMenu("View");
    AddItem(view, "Zoom In", MSG_ZOOM_IN, '+', B_COMMAND_KEY);
    AddItem(view, "Zoom Out", MSG_ZOOM_OUT, '-', B_COMMAND_KEY);
    view->AddSeparatorItem();
    BMenuItem *mix_win = new BMenuItem("Open Mixer in Window", new BMessage(MSG_MIXER_OPEN_WINDOW));
    mix_win->SetMarked(settings_get_uint32("mixer_in_window", 0) != 0);
    view->AddItem(mix_win);
    view->AddSeparatorItem();
    AddItem(view, "Metronome…", MSG_METRO_VOLUME);
    bar->AddItem(view);

    // Options — I/O routing, plugins (excluded on Haiku), MIDI control surface.
    BMenu *options = new BMenu("Options");
    AddItem(options, "Inputs/Outputs…", MSG_OPT_IO);
    AddItem(options, "Rescan Plugins", MSG_OPT_PLUGINS);
    AddItem(options, "MIDI Control…", MSG_OPT_MIDI_CONTROL);
    bar->AddItem(options);

    return bar;
}

void MainWindow::TransportPlay()
{
    if (!jackdaw_engine_begin_countin(jackdaw_project_get_countin_before_play(m_project), FALSE))
        jackdaw_engine_start_playback();
}

void MainWindow::TransportStop()
{
    jackdaw_engine_stop_recording();
    jackdaw_engine_stop_playback();
}

void MainWindow::TransportRecord()
{
    if (jackdaw_engine_is_recording() || jackdaw_engine_is_counting_in()) {
        TransportStop();
        return;
    }
    if (!jackdaw_engine_begin_countin(jackdaw_project_get_countin_before_record(m_project), TRUE))
        jackdaw_engine_start_recording();
}

void MainWindow::TransportToggle()
{
    if (jackdaw_engine_is_playing() || jackdaw_engine_is_counting_in())
        TransportStop();
    else
        TransportPlay();
}

void MainWindow::TransportPause()
{
    // Stop rolling but keep the play position (unlike RTZ / Stop-to-start).
    jackdaw_engine_stop_recording();
    jackdaw_engine_stop_playback();
}

void MainWindow::StepCursor(int direction)
{
    // One step = 10 ms (samplerate / 100), matching the Linux |<< / >>| buttons.
    jack_nframes_t sr = jackdaw_engine_get_sample_rate();
    off_t step = (off_t)(sr / 100);
    off_t pos = jackdaw_engine_get_play_pos();
    if (direction < 0)
        pos = (pos > step) ? pos - step : 0;
    else
        pos += step;
    m_timeline->LocateTo(pos);
}

void MainWindow::LocateNextBoundary()
{
    // Until clips exist (region-boundary scan lands with the clip phase), the
    // meaningful boundaries are the loop-region edges. Jump to the next one
    // after the cursor.
    off_t cursor = jackdaw_engine_get_play_pos();
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    off_t next = -1;
    if (jackdaw_engine_has_loop_region()) {
        if (ls > cursor)
            next = ls;
        else if (le > cursor)
            next = le;
    }
    if (next >= 0)
        m_timeline->LocateTo(next);
}

void MainWindow::AddTrack(JackDawTrackKind kind)
{
    JackDawTrack *t = jackdaw_track_new(NULL, NULL);
    jackdaw_track_set_kind(t, kind);
    // Engine first so track->slot is assigned before views build for it.
    if (jackdaw_engine_add_track(t)) {
        g_object_unref(t);
        return;
    }
    jackdaw_project_add_track(m_project, t); // takes its own ref
    jackdaw_project_select_single(m_project, t);
    g_object_unref(t);
}

void MainWindow::DeleteTrack(JackDawTrack *track)
{
    if (!track)
        return;
    // Close the track's piano-roll editor (async — the editor holds its own
    // GObject ref on the track, so it stays valid until the window dies).
    MidiWindow *mw = (MidiWindow *)g_object_get_data(G_OBJECT(track), "midi-window");
    if (mw)
        mw->PostMessage(B_QUIT_REQUESTED);
    // Likewise close the track's FX window (it also holds its own GObject ref).
    FxWindow *fw = (FxWindow *)g_object_get_data(G_OBJECT(track), "fx-window");
    if (fw)
        fw->PostMessage(B_QUIT_REQUESTED);
    // Clear the RT slot before dropping the last GObject ref.
    jackdaw_engine_remove_track(track);
    jackdaw_project_remove_track(m_project, track);
}

bool MainWindow::TrackInProject(JackDawTrack *track) const
{
    guint n = jackdaw_project_track_count(m_project);
    for (guint i = 0; i < n; i++)
        if (jackdaw_project_get_track(m_project, i) == track)
            return true;
    return false;
}

void MainWindow::OpenMidiEditor(JackDawTrack *track)
{
    if (!track || !jackdaw_track_is_instrument(track))
        return;
    MidiWindow *w = (MidiWindow *)g_object_get_data(G_OBJECT(track), "midi-window");
    if (w) {
        w->PostMessage(MSG_MIDI_EDITOR_PRESENT);
        return;
    }
    w = new MidiWindow(track, m_project, this);
    g_object_set_data(G_OBJECT(track), "midi-window", w);
    m_midi_editors.push_back(w);
    w->Show();
}

void MainWindow::UnregisterMidiEditor(MidiWindow *w)
{
    for (size_t i = 0; i < m_midi_editors.size(); i++) {
        if (m_midi_editors[i] == w) {
            m_midi_editors.erase(m_midi_editors.begin() + i);
            return;
        }
    }
}

// One FX window per track (like the MIDI editors): raise it if already open,
// else create it. Both the track strip and the mixer strip route here so a
// track never ends up with two FX windows.
void MainWindow::OpenFxEditor(JackDawTrack *track)
{
    if (!track)
        return;
    FxWindow *w = (FxWindow *)g_object_get_data(G_OBJECT(track), "fx-window");
    if (w) {
        w->PostMessage('fxrs'); // MSG_FX_RAISE: re-activate the existing window
        return;
    }
    w = new FxWindow(track, this);
    g_object_set_data(G_OBJECT(track), "fx-window", w);
    m_fx_windows.push_back(w);
    w->Show();
}

void MainWindow::UnregisterFxWindow(FxWindow *w)
{
    for (size_t i = 0; i < m_fx_windows.size(); i++) {
        if (m_fx_windows[i] == w) {
            m_fx_windows.erase(m_fx_windows.begin() + i);
            return;
        }
    }
}

void MainWindow::CloseAllFxWindows()
{
    for (size_t i = 0; i < m_fx_windows.size(); i++)
        m_fx_windows[i]->PostMessage(B_QUIT_REQUESTED);
    for (int spin = 0; !m_fx_windows.empty() && spin < 2000; spin++) {
        Unlock();
        snooze(1000);
        Lock();
    }
}

void MainWindow::LoadFileAsTrack(const char *path)
{
    if (!path || !*path)
        return;
    GError *err = NULL;
    AudioClip *clip = audio_clip_new(path, &err);
    if (!clip) {
        BString msg("Could not load audio file:\n");
        msg << (err ? err->message : path);
        if (err)
            g_error_free(err);
        (new BAlert("Load error", msg.String(), "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT))
            ->Go();
        return;
    }

    // Name the track after the file's leaf. Engine first (allocates the slot +
    // playback ringbuffers) so views build against a fully-wired track.
    BPath bp(path);
    JackDawTrack *t = jackdaw_track_new(bp.Leaf(), NULL);
    if (jackdaw_engine_add_track(t)) {
        audio_clip_free(clip);
        g_object_unref(t);
        return;
    }
    jackdaw_track_place_clip(t, clip, 0); // consumes the clip reference
    jackdaw_project_add_track(m_project, t);
    jackdaw_project_select_single(m_project, t);
    g_object_unref(t);
}

// ---- Project save / load ---------------------------------------------------

void MainWindow::UpdateTitle()
{
    const gchar *f = jackdaw_project_get_file(m_project);
    if (f && *f) {
        gchar *base = g_path_get_basename(f);
        BString title(base);
        if (title.EndsWith(".jdaw"))
            title.Truncate(title.Length() - 5);
        title << " — JackDAW";
        SetTitle(title.String());
        g_free(base);
    } else {
        SetTitle("JackDAW");
    }
}

void MainWindow::SaveProjectTo(const char *path)
{
    if (!path || !*path)
        return;
    if (jackdaw_project_save(m_project, path)) {
        BString msg("Could not save project to:\n");
        msg << path;
        (new BAlert("Save error", msg.String(), "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT))
            ->Go();
        return;
    }
    UpdateTitle();
    const gchar *saved = jackdaw_project_get_file(m_project);
    if (saved && *saved)
        settings_set_string("last_project", saved);
    settings_save();
}

void MainWindow::ShowSaveAsPanel()
{
    if (!m_save_panel)
        m_save_panel = new BFilePanel(B_SAVE_PANEL, new BMessenger(this), NULL, 0, false);
    gchar *dir = jackdaw_default_projects_dir();
    BEntry dent(dir, true);
    entry_ref dref;
    if (dent.GetRef(&dref) == B_OK)
        m_save_panel->SetPanelDirectory(&dref);
    g_free(dir);
    const gchar *cur = jackdaw_project_get_file(m_project);
    if (cur && *cur) {
        gchar *base = g_path_get_basename(cur);
        BString leaf(base);
        if (leaf.EndsWith(".jdaw"))
            leaf.Truncate(leaf.Length() - 5);
        m_save_panel->SetSaveText(leaf.String());
        g_free(base);
    } else {
        m_save_panel->SetSaveText("Untitled");
    }
    m_save_panel->Show();
}

void MainWindow::ShowOpenPanel()
{
    if (!m_open_panel) {
        m_open_panel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this), NULL, B_FILE_NODE, false);
        BMessage msg(MSG_OPEN_PROJECT_REFS); // distinct from load-file B_REFS_RECEIVED
        m_open_panel->SetMessage(&msg);
    }
    gchar *dir = jackdaw_default_projects_dir();
    BEntry dent(dir, true);
    entry_ref dref;
    if (dent.GetRef(&dref) == B_OK)
        m_open_panel->SetPanelDirectory(&dref);
    g_free(dir);
    m_open_panel->Show();
}

// Async-quit every piano-roll editor and drain the registry. Mirrors the
// QuitRequested handshake: editors lock THIS window for model access, so we
// only PostMessage (never lock them) and drop our lock while they tear down.
void MainWindow::CloseAllMidiEditors()
{
    for (size_t i = 0; i < m_midi_editors.size(); i++)
        m_midi_editors[i]->PostMessage(B_QUIT_REQUESTED);
    for (int spin = 0; !m_midi_editors.empty() && spin < 2000; spin++) {
        Unlock();
        snooze(1000);
        Lock();
    }
}

void MainWindow::OpenProject(const char *path)
{
    if (!path || !*path)
        return;
    // Load mutates tracks/engine slots + rebuilds FX chains; do it from a quiet
    // transport with the editors gone and stale selections cleared.
    jackdaw_engine_stop_playback();
    jackdaw_engine_locate(0);
    CloseAllMidiEditors();
    CloseAllFxWindows();
    if (m_timeline && m_timeline->TrackArea())
        m_timeline->TrackArea()->ClearSectionSelection();
    jackdaw_project_clear_selection(m_project);

    if (jackdaw_project_load(m_project, path)) {
        BString msg("Could not open project (missing or corrupt file):\n");
        msg << path;
        (new BAlert("Open error", msg.String(), "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT))
            ->Go();
        return;
    }
    // track-added / timing-changed signals emitted during load already rebuilt
    // the strips, lanes and mixers; refresh the canvas + title.
    if (m_timeline)
        m_timeline->InvalidateAll();
    UpdateTitle();
    SendMidiCtlSnapshot(); // the load replaced the mapping table
    settings_set_string("last_project", path);
    settings_save();
}

void MainWindow::NewSession()
{
    jackdaw_engine_stop_playback();
    jackdaw_engine_locate(0);
    CloseAllMidiEditors();
    CloseAllFxWindows();
    if (m_timeline && m_timeline->TrackArea())
        m_timeline->TrackArea()->ClearSectionSelection();
    jackdaw_project_clear_selection(m_project);

    // Undo mementos reference the tracks we are about to remove.
    undo_manager_clear(jackdaw_project_get_undo(m_project));

    // Hold the RT graph off the plugins while the FX chains are torn down.
    jackdaw_engine_set_suspended(TRUE);
    guint cur = jackdaw_project_track_count(m_project);
    while (cur-- > 0) {
        JackDawTrack *t = jackdaw_project_get_track(m_project, 0);
        jackdaw_engine_remove_track(t);
        jackdaw_project_remove_track(m_project, t);
    }
    jackdaw_engine_set_suspended(FALSE);

    midicontrol_clear(); // mappings reference the tracks just removed
    SendMidiCtlSnapshot();

    jackdaw_project_set_master_volume(m_project, 1.0f);
    jackdaw_project_set_master_muted(m_project, FALSE);
    jackdaw_project_set_file(m_project, NULL);
    UpdateTitle();
    if (m_timeline)
        m_timeline->InvalidateAll();
}

// ---- Render / export (P11) -------------------------------------------------
// The dialog runs its own looper and never calls the engine; it posts options
// here (MSG_RENDER_START) and this looper owns the whole render lifecycle so
// every engine non-RT call (suspend, locate, transport, tap) has a single
// caller. Progress is posted back to the dialog as MSG_RENDER_PROGRESS.

static const bigtime_t kRenderTickUsec = 66000; // ~15 Hz progress poll

void MainWindow::OpenRenderDialog(bool region)
{
    if (!m_render_window)
        m_render_window = new RenderWindow(m_project, BMessenger(this));
    if (m_render_window->LockLooper()) {
        m_render_window->Present(region);
        m_render_window->UnlockLooper();
    }
}

void MainWindow::StartRenderFromMessage(BMessage *msg)
{
    if (m_render_active)
        return; // one render at a time

    RenderOptions o;
    memset(&o, 0, sizeof o);
    o.format = (RenderFormat)msg->GetInt32("format", RENDER_FMT_WAV);
    o.bit_depth = (RenderBitDepth)msg->GetInt32("bit_depth", RENDER_BITS_24);
    o.source = (RenderSource)msg->GetInt32("source", RENDER_SRC_MASTER);
    o.scope = (RenderScope)msg->GetInt32("scope", RENDER_SCOPE_PROJECT);
    o.method = (RenderMethod)msg->GetInt32("method", RENDER_METHOD_OFFLINE);
    o.sample_rate = msg->GetInt32("sample_rate", 48000);
    o.channels = msg->GetInt32("channels", 2);
    o.out_path = g_strdup(msg->GetString("out_path", ""));
    o.project = m_project;
    o.selected_tracks = NULL;
    if (o.source == RENDER_SRC_SELECTED) {
        GPtrArray *sel = jackdaw_project_get_selected_tracks(m_project);
        o.selected_tracks = g_ptr_array_new();
        for (guint i = 0; sel && i < sel->len; i++)
            g_ptr_array_add(o.selected_tracks, g_ptr_array_index(sel, i));
    }

    if (!o.out_path || !*o.out_path || !jackdaw_render_format_supported(&o)) {
        render_options_free_contents(&o);
        SendRenderProgress(3); // failed
        return;
    }

    // Region scope with no loop tabs -> empty span; reject before starting.
    if (o.scope == RENDER_SCOPE_REGION && !jackdaw_engine_has_loop_region()) {
        render_options_free_contents(&o);
        SendRenderProgress(3);
        return;
    }

    // The engine must be quiet before a render takes over the graph/transport.
    jackdaw_engine_stop_playback();

    memset(&m_render_prog, 0, sizeof m_render_prog);
    m_render_method = o.method;
    m_render_thread = NULL;

    if (o.method == RENDER_METHOD_OFFLINE) {
        m_render_thread = jackdaw_render_offline_start(&o, &m_render_prog);
    } else {
        if (jackdaw_render_realtime_start(&o, &m_render_prog)) {
            render_options_free_contents(&o);
            SendRenderProgress(3);
            return;
        }
    }
    render_options_free_contents(&o); // both start paths deep-copied the options

    m_render_active = true;
    m_render_tick =
        new BMessageRunner(BMessenger(this), BMessage(MSG_RENDER_TICK), kRenderTickUsec);
}

void MainWindow::RenderTick()
{
    if (!m_render_active)
        return;
    if (m_render_method == RENDER_METHOD_REALTIME)
        jackdaw_render_realtime_poll(&m_render_prog);

    if (!g_atomic_int_get(&m_render_prog.finished)) {
        SendRenderProgress(0); // running
        return;
    }

    bool cancelled = g_atomic_int_get(&m_render_prog.cancel) != 0;
    bool failed = g_atomic_int_get(&m_render_prog.failed) != 0;
    CleanupRender();
    SendRenderProgress(failed ? 3 : (cancelled ? 2 : 1));
}

void MainWindow::SendRenderProgress(int state)
{
    if (!m_render_window)
        return;
    double frac = 1.0;
    off_t total = m_render_prog.frames_total;
    off_t done = m_render_prog.frames_done;
    if (total > 0) {
        frac = (double)done / (double)total;
        if (frac < 0.0)
            frac = 0.0;
        if (frac > 1.0)
            frac = 1.0;
    }
    BMessage p(MSG_RENDER_PROGRESS);
    p.AddDouble("frac", frac);
    p.AddInt32("state", state);
    BMessenger(m_render_window).SendMessage(&p);
}

void MainWindow::CleanupRender()
{
    if (m_render_tick) {
        delete m_render_tick;
        m_render_tick = NULL;
    }
    if (m_render_thread) {
        g_thread_join(m_render_thread); // finished flag already set by the worker
        m_render_thread = NULL;
    }
    m_render_active = false;
}

void MainWindow::ShowTrackContext(int slot, BPoint screen_where)
{
    JackDawTrack *t = NULL;
    guint n = jackdaw_project_track_count(m_project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *cand = jackdaw_project_get_track(m_project, i);
        if ((int)cand->slot == slot) {
            t = cand;
            break;
        }
    }
    if (!t)
        return;

    BPopUpMenu menu("track_context", false, false);
    BMenuItem *del = new BMenuItem("Delete Track", NULL);
    menu.AddItem(del);
    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == del)
        DeleteTrack(t);
}

void MainWindow::ShowTrackAreaContext(BPoint screen_where)
{
    // Mirrors the Linux track-container context menu, minus "Show Master
    // Track" — this port has no master row, only master_volume/master_muted
    // project fields and the mixer's master strip.
    BPopUpMenu menu("track_area_context", false, false);
    BMenuItem *add = new BMenuItem("Add Track", NULL);
    BMenuItem *add_midi = new BMenuItem("Add MIDI Track", NULL);
    menu.AddItem(add);
    menu.AddItem(add_midi);
    BMenuItem *chosen = menu.Go(screen_where, false, false);
    // Post rather than calling AddTrack directly so both entry points (menu bar
    // and popup) go through the same handler.
    if (chosen == add)
        PostMessage(MSG_TRACK_ADD);
    else if (chosen == add_midi)
        PostMessage(MSG_TRACK_ADD_MIDI);
}

void MainWindow::ShowRecordMenu(BPoint screen_where)
{
    BPopUpMenu menu("record_mode", false, false);
    BMenuItem *normal = new BMenuItem("Normal", new BMessage(MSG_RECORD_MODE_NORMAL));
    BMenuItem *punch = new BMenuItem("Punch In/Out", new BMessage(MSG_RECORD_MODE_PUNCH));
    int mode = jackdaw_engine_get_record_mode();
    normal->SetMarked(mode != RECORD_MODE_PUNCH);
    punch->SetMarked(mode == RECORD_MODE_PUNCH);
    menu.AddItem(normal);
    menu.AddItem(punch);

    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == normal)
        jackdaw_engine_set_record_mode(RECORD_MODE_NORMAL);
    else if (chosen == punch)
        jackdaw_engine_set_record_mode(RECORD_MODE_PUNCH);
}

void MainWindow::ShowMetroMenu(BPoint screen_where)
{
    BPopUpMenu menu("metro_options", false, false);
    BMenuItem *vol = new BMenuItem("Volume…", NULL);
    BMenuItem *ci = new BMenuItem("Count in…", NULL);
    BMenuItem *hp = new BMenuItem("Headphones only (click output)", NULL);
    hp->SetMarked(jackdaw_project_get_metronome_route(m_project) == METRONOME_ROUTE_CLICK_PORT);
    menu.AddItem(vol);
    menu.AddItem(ci);
    menu.AddSeparatorItem();
    menu.AddItem(hp);

    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == vol) {
        OpenMetroVolumeWindow();
    } else if (chosen == ci) {
        OpenCountInWindow();
    } else if (chosen == hp) {
        JackDawMetronomeRoute route =
            hp->IsMarked() ? METRONOME_ROUTE_MAIN : METRONOME_ROUTE_CLICK_PORT;
        jackdaw_project_set_metronome_route(m_project, route);
    }
}

void MainWindow::ShowMixerMenu(BPoint screen_where)
{
    BPopUpMenu menu("mixer_options", false, false);
    BMenuItem *in_window = new BMenuItem("Open in Window", NULL);
    in_window->SetMarked(settings_get_uint32("mixer_in_window", 0) != 0);
    menu.AddItem(in_window);

    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == in_window)
        PostMessage(MSG_MIXER_OPEN_WINDOW);
}

void MainWindow::OpenMetroVolumeWindow()
{
    if (m_metro_volume_window == NULL)
        m_metro_volume_window = new MetroVolumeWindow(BMessenger(this));
    // The option window runs its own looper; lock it before touching its views.
    if (m_metro_volume_window->LockLooper()) {
        m_metro_volume_window->SyncVolume(jackdaw_project_get_metronome_volume(m_project));
        if (m_metro_volume_window->IsHidden())
            m_metro_volume_window->Show();
        m_metro_volume_window->Activate();
        m_metro_volume_window->UnlockLooper();
    }
}

void MainWindow::OpenCountInWindow()
{
    if (m_countin_window == NULL)
        m_countin_window = new CountInWindow(BMessenger(this));
    if (m_countin_window->LockLooper()) {
        m_countin_window->SyncCountin((int)jackdaw_project_get_countin_before_record(m_project),
                                      (int)jackdaw_project_get_countin_before_play(m_project));
        if (m_countin_window->IsHidden())
            m_countin_window->Show();
        m_countin_window->Activate();
        m_countin_window->UnlockLooper();
    }
}

void MainWindow::OpenIoWindow()
{
    if (m_io_window == NULL)
        m_io_window = new IoWindow(BMessenger(this));
    if (m_io_window->LockLooper()) {
        m_io_window->SyncCounts((int)jackdaw_engine_get_audio_in_count(),
                                (int)jackdaw_engine_get_audio_out_count());
        if (m_io_window->IsHidden())
            m_io_window->Show();
        m_io_window->Activate();
        m_io_window->UnlockLooper();
    }
}

void MainWindow::OpenMidiControlWindow()
{
    if (m_midictl_window == NULL)
        m_midictl_window = new MidiControlWindow(BMessenger(this));
    if (m_midictl_window->LockLooper()) {
        if (m_midictl_window->IsHidden())
            m_midictl_window->Show();
        m_midictl_window->Activate();
        m_midictl_window->UnlockLooper();
    }
    SendMidiCtlSnapshot(); // populate/refresh the dialog's local view
}

void MainWindow::SendMidiCtlSnapshot()
{
    if (m_midictl_window == NULL)
        return;

    BMessage snap(MSG_MIDICTL_SNAPSHOT);
    guint n = midicontrol_count();
    for (guint i = 0; i < n; i++) {
        MidiCtlMapping *m = midicontrol_get(i);
        if (m)
            snap.AddData("map", B_RAW_TYPE, m, sizeof(*m));
    }
    guint tc = jackdaw_project_track_count(m_project);
    for (guint t = 0; t < tc; t++) {
        JackDawTrack *tr = jackdaw_project_get_track(m_project, t);
        snap.AddString("track", tr ? jackdaw_track_get_name(tr) : "");
    }
    const char **ports = jackdaw_engine_list_midi_ports();
    if (ports) {
        for (int p = 0; ports[p]; p++)
            snap.AddString("port", ports[p]);
        jackdaw_engine_free_ports(ports);
    }
    const gchar *src = jackdaw_engine_get_control_source();
    snap.AddString("cursource", src ? src : "");
    snap.AddInt32("learn", midicontrol_get_learn());

    BMessenger(m_midictl_window).SendMessage(&snap);
}

void MainWindow::MidiControlPoll()
{
    // Drain every buffered control event and dispatch it against the mappings.
    // Bounded by the ring's finite fill; dispatch touches pluginhost/track/
    // transport, all of which are this looper's to call.
    JackDawCtlEvent ev;
    while (jackdaw_engine_control_poll(&ev))
        midicontrol_dispatch_event(m_project, ev.data, ev.size);
}

void MainWindow::MidiCtlTransport(int which)
{
    switch (which) {
        case ACT_TRANSPORT_PLAY:
            TransportToggle();
            break;
        case ACT_TRANSPORT_STOP:
            TransportStop();
            break;
        case ACT_TRANSPORT_REC:
            TransportRecord();
            break;
        default:
            break;
    }
}

void MainWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_UI_TICK:
            m_transport->UpdateReadout();
            m_timeline->TickUpdate();
            SyncMixers();
            break;

        case MSG_ZOOM_IN:
            m_timeline->ZoomBy(0.5, m_timeline->LaneWidth() / 2);
            break;
        case MSG_ZOOM_OUT:
            m_timeline->ZoomBy(2.0, m_timeline->LaneWidth() / 2);
            break;

        case MSG_TRANSPORT_PLAY:
            TransportPlay();
            break;
        case MSG_TRANSPORT_STOP:
            TransportStop();
            break;
        case MSG_TRANSPORT_RECORD:
            TransportRecord();
            break;
        case MSG_TRANSPORT_TOGGLE:
            TransportToggle();
            break;
        case MSG_TRANSPORT_PAUSE:
            TransportPause();
            break;
        case MSG_TRANSPORT_RTZ:
            jackdaw_engine_locate(0);
            // Return the view to the start with the playhead (mirrors the MIDI
            // window's LocateStart, which resets its horizontal origin too).
            if (m_timeline)
                m_timeline->SetViewStart(0);
            break;
        case MSG_TRANSPORT_STEP_BACK:
            StepCursor(-1);
            break;
        case MSG_TRANSPORT_STEP_FWD:
            StepCursor(+1);
            break;
        case MSG_TRANSPORT_NEXT_BOUNDARY:
            LocateNextBoundary();
            break;
        case MSG_TRANSPORT_LOOP:
            jackdaw_engine_set_loop_enabled(!jackdaw_engine_get_loop_enabled());
            m_timeline->InvalidateAll();
            break;

        case MSG_SPLIT:
            m_timeline->TrackArea()->SplitAtCursor();
            break;
        case MSG_REGION_COPY:
            m_timeline->TrackArea()->CopySelection();
            break;
        case MSG_REGION_PASTE:
            m_timeline->TrackArea()->PasteAtCursor();
            break;
        case MSG_REGION_DELETE:
            m_timeline->TrackArea()->DeleteSelection();
            break;
        case MSG_REGION_GROUP:
            m_timeline->TrackArea()->GroupSelection();
            break;

        case MSG_TRACK_LOAD_FILE: {
            if (!m_load_panel)
                m_load_panel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this), NULL, B_FILE_NODE,
                                              true /* allow multi-select */);
            m_load_panel->Show();
            break;
        }
        case B_REFS_RECEIVED: {
            // Selections from the "Load File as New Track" open panel.
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
                BEntry entry(&ref, true);
                BPath path;
                if (entry.GetPath(&path) == B_OK)
                    LoadFileAsTrack(path.Path());
            }
            break;
        }

        // ---- Project save / load ----
        case MSG_FILE_SAVE: {
            const gchar *cur = jackdaw_project_get_file(m_project);
            if (cur && *cur)
                SaveProjectTo(cur);
            else
                ShowSaveAsPanel();
            break;
        }
        case MSG_FILE_SAVE_AS:
            ShowSaveAsPanel();
            break;
        case B_SAVE_REQUESTED: {
            // Result of the "Save Project As" save panel: directory + name.
            entry_ref dir;
            const char *name = NULL;
            if (message->FindRef("directory", &dir) == B_OK &&
                message->FindString("name", &name) == B_OK && name && *name) {
                BPath path(&dir);
                path.Append(name);
                SaveProjectTo(path.Path());
            }
            break;
        }
        case MSG_FILE_OPEN:
            ShowOpenPanel();
            break;
        case MSG_OPEN_PROJECT_REFS: {
            // Selection from the "Open Project" panel (a .jdaw file).
            entry_ref ref;
            if (message->FindRef("refs", 0, &ref) == B_OK) {
                BEntry entry(&ref, true);
                BPath path;
                if (entry.GetPath(&path) == B_OK)
                    OpenProject(path.Path());
            }
            break;
        }
        case MSG_FILE_NEW:
            NewSession();
            break;

        case MSG_FILE_RENDER:
            OpenRenderDialog(false);
            break;
        case MSG_FILE_RENDER_REGION:
            OpenRenderDialog(true);
            break;

        case MSG_RENDER_START:
            StartRenderFromMessage(message);
            break;
        case MSG_RENDER_CANCEL:
            if (m_render_active)
                g_atomic_int_set(&m_render_prog.cancel, 1);
            break;
        case MSG_RENDER_TICK:
            RenderTick();
            break;

        case MSG_EDIT_UNDO:
            // The section-selection pointers reference the pre-undo region list;
            // drop them so the restored regions aren't touched through stale ptrs.
            m_timeline->TrackArea()->ClearSectionSelection();
            jackdaw_project_undo(m_project);
            break;
        case MSG_EDIT_REDO:
            m_timeline->TrackArea()->ClearSectionSelection();
            jackdaw_project_redo(m_project);
            break;

        case MSG_TRACK_ADD:
            AddTrack(JACKDAW_TRACK_AUDIO);
            break;
        case MSG_TRACK_ADD_MIDI:
            AddTrack(JACKDAW_TRACK_INSTRUMENT);
            break;
        case MSG_TRACK_DELETE:
            DeleteTrack(jackdaw_project_get_active_track(m_project));
            break;
        case MSG_TRACK_DELETE_SLOT:
        case MSG_TRACK_CONTEXT: {
            int32 slot = -1;
            message->FindInt32("slot", &slot);
            if (message->what == MSG_TRACK_DELETE_SLOT) {
                guint n = jackdaw_project_track_count(m_project);
                for (guint i = 0; i < n; i++) {
                    JackDawTrack *t = jackdaw_project_get_track(m_project, i);
                    if ((int)t->slot == slot) {
                        DeleteTrack(t);
                        break;
                    }
                }
            } else {
                BPoint where;
                if (message->FindPoint("screen_where", &where) == B_OK)
                    ShowTrackContext(slot, where);
            }
            break;
        }
        case MSG_TRACK_AREA_CONTEXT: {
            BPoint where;
            if (message->FindPoint("screen_where", &where) == B_OK)
                ShowTrackAreaContext(where);
            break;
        }
        case MSG_TRACK_MOVE: {
            int32 from = -1, to = -1;
            message->FindInt32("from", &from);
            message->FindInt32("to", &to);
            if (from >= 0 && to >= 0 && from != to) {
                jackdaw_project_move_track(m_project, (guint)from, (guint)to);
                jackdaw_project_emit_tracks_reordered(m_project); // resync the row order
            }
            break;
        }

        // Right-click context popups (screen point carried in the message).
        case MSG_RECORD_MENU:
        case MSG_METRO_MENU:
        case MSG_MIXER_MENU: {
            BPoint where;
            if (message->FindPoint("screen_where", &where) != B_OK)
                break;
            if (message->what == MSG_RECORD_MENU)
                ShowRecordMenu(where);
            else if (message->what == MSG_METRO_MENU)
                ShowMetroMenu(where);
            else
                ShowMixerMenu(where);
            break;
        }

        case MSG_METRO_VOLUME:
            OpenMetroVolumeWindow();
            break;
        case MSG_METRO_COUNTIN:
            OpenCountInWindow();
            break;

        // Value changes applied on this looper (posted by the option windows).
        case MSG_METRO_SET_VOLUME: {
            float db = 0.0f;
            message->FindFloat("db", &db);
            jackdaw_project_set_metronome_volume(m_project, db);
            break;
        }
        case MSG_METRO_SET_COUNTIN_REC: {
            int32 beats = 0;
            message->FindInt32("beats", &beats);
            jackdaw_project_set_countin_before_record(m_project, (guint)(beats < 0 ? 0 : beats));
            break;
        }
        case MSG_METRO_SET_COUNTIN_PLAY: {
            int32 beats = 0;
            message->FindInt32("beats", &beats);
            jackdaw_project_set_countin_before_play(m_project, (guint)(beats < 0 ? 0 : beats));
            break;
        }

        case MSG_OPT_IO:
            OpenIoWindow();
            break;

        // Applied on this looper (posted by IoWindow): resize the JACK capture
        // (in_N) and master output (out_N) port pools. The engine's port-
        // registration callback then posts MSG_ENGINE_PORTS_CHANGED, which
        // refreshes the strips; do it here too so the update is immediate.
        case MSG_OPT_IO_APPLY: {
            int32 inputs = 0, outputs = 0;
            message->FindInt32("inputs", &inputs);
            message->FindInt32("outputs", &outputs);
            if (inputs > 0)
                jackdaw_engine_set_audio_in_count((guint)inputs);
            if (outputs > 0)
                jackdaw_engine_set_audio_out_count((guint)outputs);
            if (m_timeline && m_timeline->TrackArea())
                m_timeline->TrackArea()->RefreshInputMenus();
            break;
        }

        case MSG_OPT_MIDI_CONTROL:
            OpenMidiControlWindow();
            break;

        // Control-surface poll tick: drain control_in and dispatch mappings.
        case MSG_MIDICTL_POLL:
            MidiControlPoll();
            break;

        // Dialog -> here (single mutator for the mapping table + engine). Each
        // applies the edit, then pushes a fresh snapshot back to the dialog.
        case MSG_MIDICTL_ADD:
            midicontrol_add(NULL);
            SendMidiCtlSnapshot();
            break;
        case MSG_MIDICTL_REMOVE: {
            int32 index = -1;
            message->FindInt32("index", &index);
            if (index >= 0)
                midicontrol_remove((guint)index);
            SendMidiCtlSnapshot();
            break;
        }
        case MSG_MIDICTL_LEARN: {
            int32 index = -1;
            message->FindInt32("index", &index);
            midicontrol_set_learn(index);
            SendMidiCtlSnapshot();
            break;
        }
        case MSG_MIDICTL_UPDATE: {
            int32 index = -1;
            message->FindInt32("index", &index);
            MidiCtlMapping *m = index >= 0 ? midicontrol_get((guint)index) : NULL;
            if (m) {
                int32 v;
                if (message->FindInt32("msg_type", &v) == B_OK)
                    m->msg_type = (guint8)v;
                if (message->FindInt32("channel", &v) == B_OK)
                    m->channel = (gint8)v;
                if (message->FindInt32("number", &v) == B_OK)
                    m->number = (guint8)v;
                if (message->FindInt32("action", &v) == B_OK)
                    m->action = (guint8)CLAMP(v, 0, ACT_NACTIONS - 1);
                if (message->FindInt32("track", &v) == B_OK)
                    m->track_index = v;
                if (message->FindInt32("fx", &v) == B_OK)
                    m->fx_index = v;
                if (message->FindInt32("param", &v) == B_OK)
                    m->param_index = v;
                if (message->FindInt32("group", &v) == B_OK)
                    m->switch_group = v;
            }
            // No snapshot: the dialog already reflects these edits locally, and
            // echoing back would fight the user's in-flight menu/stepper focus.
            break;
        }
        case MSG_MIDICTL_SOURCE: {
            const char *port = NULL;
            message->FindString("port", &port);
            jackdaw_engine_set_control_source(port && *port ? port : NULL);
            SendMidiCtlSnapshot();
            break;
        }

        case MSG_OPT_PLUGINS: {
            // Rescan now, behind the same progress window the startup scan
            // uses. It runs on its own looper, so it keeps repainting while
            // this looper is busy scanning; we only ever post to it.
            ScanWindow *scan = new ScanWindow(pluginhost_scan_candidate_count());
            BMessenger scan_msgr(scan);
            scan->Show();
            pluginhost_set_scan_progress(&ScanWindow::ProgressHook, &scan_msgr);
            pluginhost_rescan();
            pluginhost_set_scan_progress(NULL, NULL);
            scan->PostMessage(B_QUIT_REQUESTED);

            guint n = g_list_length((GList *)pluginhost_catalog());
            char text[96];
            snprintf(text, sizeof(text), "Plugin rescan complete: %u effects found.", n);
            BAlert *alert = new BAlert("plugins", text, "OK");
            alert->Go(NULL); // async; the alert deletes itself
            break;
        }

        case MSG_MIXER_TOGGLE:
            m_mixer_visible = !m_mixer_visible;
            ApplyMixerState();
            break;
        case MSG_MIXER_OPEN_WINDOW: {
            // Toggle docked <-> detached. Enabling the window also shows the
            // mixer if it was off; closing the window (which re-posts this)
            // flips back to docked.
            bool on = settings_get_uint32("mixer_in_window", 0) != 0;
            settings_set_uint32("mixer_in_window", on ? 0 : 1);
            if (!on)
                m_mixer_visible = true;
            ApplyMixerState();
            break;
        }

        // Mixer strip edits (docked or detached) applied here (single mutator).
        case MSG_MIX_SET_FADER: {
            int32 slot = -1;
            float gain = 1.0f;
            message->FindInt32("slot", &slot);
            message->FindFloat("gain", &gain);
            if (slot < 0)
                jackdaw_project_set_master_volume(m_project, gain);
            else if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_fader(t, gain);
            break;
        }
        case MSG_MIX_SET_PAN: {
            int32 slot = -1;
            float pan = 0.0f;
            message->FindInt32("slot", &slot);
            message->FindFloat("pan", &pan);
            if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_pan(t, pan);
            break;
        }
        case MSG_MIX_TOGGLE_MUTE: {
            int32 slot = -1;
            bool on = false;
            message->FindInt32("slot", &slot);
            message->FindBool("on", &on);
            if (slot < 0)
                jackdaw_project_set_master_muted(m_project, on);
            else if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_muted(t, on);
            break;
        }
        case MSG_MIX_TOGGLE_SOLO: {
            int32 slot = -1;
            bool on = false;
            message->FindInt32("slot", &slot);
            message->FindBool("on", &on);
            if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_soloed(t, on);
            break;
        }

        case MSG_ENGINE_PORTS_CHANGED:
        case MSG_ENGINE_CONNECTIONS_CHANGED:
            // JACK ports appeared/disappeared (device change, or our own capture
            // pool resized via Options -> Inputs/Outputs) or a connection changed:
            // rebuild every strip's input selector and re-mark the live source.
            if (m_timeline && m_timeline->TrackArea())
                m_timeline->TrackArea()->RefreshInputMenus();
            break;

        case MSG_ENGINE_SHUTDOWN:
            // A server shutdown alert can hang off this later.
            break;

        case MSG_ENGINE_TAKE_READY:
            // A recorded take is on disk: create its clip and place it on the
            // timeline (main-thread region-list edits). The lane redraws via the
            // track's state-changed signal.
            jackdaw_engine_finalize_takes();
            break;

        case MSG_ENGINE_MIDI_TAKE_READY:
            // A MIDI take was captured: drain each armed instrument track's ring
            // into clip notes and republish the RT snapshot (main-thread edits).
            jackdaw_engine_finalize_midi_takes();
            break;

        case MSG_MIDI_OPEN_EDITOR: {
            JackDawTrack *t = NULL;
            if (message->FindPointer("track", (void **)&t) == B_OK && t && TrackInProject(t))
                OpenMidiEditor(t);
            break;
        }

        case MSG_OPEN_FX: {
            JackDawTrack *t = NULL;
            if (message->FindPointer("track", (void **)&t) == B_OK && t && TrackInProject(t))
                OpenFxEditor(t);
            break;
        }

        case MSG_MIX_FX: {
            int32 slot = -1;
            message->FindInt32("slot", &slot);
            if (JackDawTrack *t = TrackForSlot(slot))
                OpenFxEditor(t);
            break;
        }

        case MSG_FX_CHAIN_CHANGED: {
            // An FX window added/removed a plug-in: refresh the owning track's
            // strip (via its state-changed signal, handled on this looper) and
            // the mixer strips (Fx tint follows fx_count).
            JackDawTrack *t = NULL;
            if (message->FindPointer("track", (void **)&t) == B_OK && t && TrackInProject(t))
                g_signal_emit_by_name(t, "state-changed");
            SyncMixers();
            break;
        }

        case MSG_MIDI_LOCATE: {
            // Seek requested from a piano-roll ruler (engine mutation runs here,
            // the single-mutator looper).
            int64 frame = 0;
            if (message->FindInt64("frame", &frame) == B_OK) {
                jackdaw_engine_locate((off_t)(frame < 0 ? 0 : frame));
                m_timeline->InvalidateAll();
            }
            break;
        }

        case MSG_MIDI_SET_LOOP: {
            int64 start = 0, end = 0;
            if (message->FindInt64("start", &start) == B_OK &&
                message->FindInt64("end", &end) == B_OK) {
                jackdaw_engine_set_loop_range((off_t)start, (off_t)end);
                bool disable = false;
                if (message->FindBool("disable", &disable) == B_OK && disable)
                    jackdaw_engine_set_loop_enabled(FALSE);
                m_timeline->InvalidateAll();
            }
            break;
        }

        case MSG_MIDI_PREVIEW: {
            // Audition a note from a piano-roll keyboard. Routed here so the
            // engine's preview ring keeps a single producer thread.
            JackDawTrack *t = NULL;
            int8 pitch = 0, vel = 0, chan = 0;
            bool on = false;
            if (message->FindInt8("channel", &chan) != B_OK)
                chan = 0;
            if (message->FindPointer("track", (void **)&t) == B_OK && t && TrackInProject(t) &&
                message->FindInt8("pitch", &pitch) == B_OK &&
                message->FindInt8("velocity", &vel) == B_OK && message->FindBool("on", &on) == B_OK)
                jackdaw_engine_preview_note(t, (guint8)pitch, (guint8)vel, (guint8)chan,
                                            on ? TRUE : FALSE);
            break;
        }

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool MainWindow::QuitRequested()
{
    // Persist the window frame for next launch.
    {
        BRect f = Frame();
        // The mixer always launches hidden, so persist the height *without* any
        // currently-docked mixer pane; otherwise the docked height would
        // accumulate into the saved frame across sessions.
        settings_set_uint32("win_x", (guint32)(gint32)f.left);
        settings_set_uint32("win_y", (guint32)(gint32)f.top);
        settings_set_uint32("win_w", (guint32)f.Width());
        settings_set_uint32("win_h", (guint32)(f.Height() - m_dock_h_applied));
        settings_save();
    }

    // Stop engine events reaching a window that is about to die. The engine
    // itself is torn down by main() after the app loop exits.
    jackdaw_engine_set_event_hook(NULL, NULL);

    // Piano-roll editors lock THIS window's looper for model access, so they
    // must be gone before this window dies — but locking them from here could
    // deadlock against an editor waiting for our lock. Instead: ask each to
    // quit (async), then drop our lock while waiting for their destructors
    // (which unregister here under our lock) to drain the registry.
    for (size_t i = 0; i < m_midi_editors.size(); i++)
        m_midi_editors[i]->PostMessage(B_QUIT_REQUESTED);
    for (int spin = 0; !m_midi_editors.empty() && spin < 2000; spin++) {
        Unlock();
        snooze(1000);
        Lock();
    }

    // FX windows likewise lock THIS window in their destructors (to unregister),
    // so they must drain the same way — async-quit each, dropping our lock while
    // waiting — before this window's looper dies. Skipping this let an open FX
    // window outlive the main window and fault when its destructor locked a dead
    // looper.
    CloseAllFxWindows();

    // Cancel and drain any in-flight render (it holds the engine suspended /
    // the transport running and a worker thread we must join before teardown).
    if (m_render_active) {
        g_atomic_int_set(&m_render_prog.cancel, 1);
        for (int spin = 0; m_render_active && spin < 4000; spin++) {
            if (m_render_method == RENDER_METHOD_REALTIME)
                jackdaw_render_realtime_poll(&m_render_prog);
            if (g_atomic_int_get(&m_render_prog.finished)) {
                CleanupRender();
                break;
            }
            snooze(1000);
        }
        CleanupRender();
    }
    // The render dialog hides rather than quits; force it closed like the mixer.
    if (m_render_window && m_render_window->Lock()) {
        m_render_window->Quit();
        m_render_window = NULL;
    }

    // Force the detached mixer window closed first (its QuitRequested normally
    // re-docks rather than quits, which would otherwise block app shutdown).
    if (m_mixer_window && m_mixer_window->Lock()) {
        m_mixer_window->Quit(); // deletes the window
        m_mixer_window = NULL;
    }
    // The metronome option singletons hide (QuitRequested returns false) so they
    // survive being reopened; force them closed here or they block app shutdown
    // the same way the detached mixer would. Quit() tears down without asking.
    if (m_metro_volume_window && m_metro_volume_window->Lock()) {
        m_metro_volume_window->Quit();
        m_metro_volume_window = NULL;
    }
    if (m_countin_window && m_countin_window->Lock()) {
        m_countin_window->Quit();
        m_countin_window = NULL;
    }
    if (m_io_window && m_io_window->Lock()) {
        m_io_window->Quit();
        m_io_window = NULL;
    }
    if (m_midictl_window && m_midictl_window->Lock()) {
        m_midictl_window->Quit();
        m_midictl_window = NULL;
    }
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}
