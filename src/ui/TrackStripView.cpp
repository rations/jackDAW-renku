#include "TrackStripView.h"

#include "StateButton.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <TextControl.h>
#include <Window.h>

#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/jackdaw-engine.h"
#include "KnobView.h"
#include "Messages.h"
#include "PortSelector.h"
#include "TimelineView.h"  // kTimelineTrackHeight (row geometry for the reorder drag)
#include "TrackAreaView.h" // parent view: drop-indicator feedback
#include "VuView.h"

// GObject signal trampolines: a track's state/routing changed elsewhere (e.g.
// the mixer) — re-read it into this strip's controls. Emitted only on the
// MainWindow looper (the sole engine mutator), so touching views is safe.
static void strip_track_changed(gpointer /*obj*/, gpointer user)
{
    static_cast<TrackStripView *>(user)->SyncFromTrack();
}

// Internal messages (this strip is the target).
enum {
    MSG_S_NAME = 'snam',
    MSG_S_ARM = 'sarm',
    MSG_S_MUTE = 'smut',
    MSG_S_SOLO = 'ssol',
    MSG_S_STEREO = 'sste',
    MSG_S_VOL = 'svol',
    MSG_S_PAN = 'span',
    MSG_S_INPUT = 'sinp',
    MSG_S_FX = 'sfx ',
};

TrackStripView::TrackStripView(JackDawProject *project, JackDawTrack *track, const BMessenger &main)
    : BView("track_strip", B_WILL_DRAW | B_FRAME_EVENTS), m_project(project), m_track(track),
      m_main(main), m_name(NULL), m_arm(NULL), m_mute(NULL), m_solo(NULL), m_stereo(NULL),
      m_fx(NULL), m_vol(NULL), m_pan(NULL), m_input(NULL), m_vu(NULL),
      m_state_handler(0), m_routing_handler(0), m_maybe_drag(false), m_did_drag(false),
      m_drag_start_y(0.0f), m_drag_from(-1)
{
    m_name =
        new BTextControl("name", NULL, jackdaw_track_get_name(track), new BMessage(MSG_S_NAME));

    m_arm = new StateButton("A", "A", new BMessage(MSG_S_ARM));
    m_mute = new StateButton("M", "M", new BMessage(MSG_S_MUTE));
    m_solo = new StateButton("S", "S", new BMessage(MSG_S_SOLO));
    m_stereo = new BButton("Mo", "Mo", new BMessage(MSG_S_STEREO));
    m_fx = new StateButton("Fx", "Fx", new BMessage(MSG_S_FX));
    m_arm->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_mute->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_solo->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_stereo->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_fx->SetToolTip("Effects (VST3 chain)");
    // Active-state colours: Arm red, Mute orange, Solo yellow (dark text), Fx
    // blue whenever the track carries an effect chain (set in SyncFromTrack).
    m_arm->SetActiveColor((rgb_color){192, 57, 43, 255}, (rgb_color){255, 255, 255, 255});
    m_mute->SetActiveColor((rgb_color){230, 126, 34, 255}, (rgb_color){255, 255, 255, 255});
    m_solo->SetActiveColor((rgb_color){255, 224, 0, 255}, (rgb_color){16, 16, 16, 255});
    m_fx->SetActiveColor((rgb_color){41, 128, 185, 255}, (rgb_color){255, 255, 255, 255});
    m_fx->SetActiveIgnoresValue(true); // momentary: blue tracks fx_count, not the click
    for (BButton *b :
         {(BButton *)m_arm, (BButton *)m_mute, (BButton *)m_solo, m_stereo, (BButton *)m_fx}) {
        b->SetExplicitMinSize(BSize(24.0f, 20.0f));
        b->SetExplicitMaxSize(BSize(30.0f, 20.0f));
    }

    double trim_db = 20.0 * log10((double)jackdaw_track_get_trim(track));
    if (!(trim_db > -25.0))
        trim_db = (jackdaw_track_get_trim(track) > 0.0001f) ? trim_db : 0.0;
    m_vol =
        new KnobView("V", -25.0, 25.0, trim_db, 0.0, KnobView::KIND_DB, new BMessage(MSG_S_VOL));
    m_vol->SetCenterLabel("V");
    m_pan = new KnobView("P", -1.0, 1.0, (double)jackdaw_track_get_pan(track), 0.0,
                         KnobView::KIND_PAN, new BMessage(MSG_S_PAN));
    m_pan->SetCenterLabel("P");

    m_input = new PortSelector("input", new BMessage(MSG_S_INPUT));

    m_vu = new VuView("vu", 14.0f);

    // Left content column + a full-height VU pinned on the right. The content
    // stacks: name, the A/M/S/Mo/Fx button row, then the V/P knobs beside the
    // input selector.
    BLayoutBuilder::Group<>(this, B_HORIZONTAL, 3.0f)
        .SetInsets(4.0f, 3.0f, 3.0f, 3.0f)
        .AddGroup(B_VERTICAL, 2.0f)
        .Add(m_name)
        .AddGroup(B_HORIZONTAL, 2.0f)
        .Add(m_arm)
        .Add(m_mute)
        .Add(m_solo)
        .Add(m_stereo)
        .Add(m_fx)
        .AddGlue()
        .End()
        .AddGroup(B_HORIZONTAL, 3.0f)
        .Add(m_vol)
        .Add(m_pan)
        .Add(m_input)
        .End()
        .AddGlue()
        .End()
        .Add(m_vu)
        .End();
}

void TrackStripView::AttachedToWindow()
{
    BView::AttachedToWindow();
    // Opaque background so app_server erases the strip (and, by inheritance, its
    // custom child views) before each Draw. Selection is shown with an accent
    // bar rather than a full-strip tint, so the knobs keep matching the bg.
    SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

    m_name->SetTarget(this);
    m_arm->SetTarget(this);
    m_mute->SetTarget(this);
    m_solo->SetTarget(this);
    m_stereo->SetTarget(this);
    m_fx->SetTarget(this);
    m_vol->SetTarget(this);
    m_pan->SetTarget(this);
    m_input->SetTarget(this);

    BuildInputMenu();
    SyncFromTrack();

    m_state_handler =
        g_signal_connect(m_track, "state-changed", G_CALLBACK(strip_track_changed), this);
    m_routing_handler =
        g_signal_connect(m_track, "routing-changed", G_CALLBACK(strip_track_changed), this);
}

void TrackStripView::DetachedFromWindow()
{
    if (m_state_handler) {
        g_signal_handler_disconnect(m_track, m_state_handler);
        m_state_handler = 0;
    }
    if (m_routing_handler) {
        g_signal_handler_disconnect(m_track, m_routing_handler);
        m_routing_handler = 0;
    }
    BView::DetachedFromWindow();
}

void TrackStripView::BuildInputMenu()
{
    // The sources the selector offers: audio capture ports for audio tracks
    // (with their stereo pairs), MIDI capture ports for instrument tracks. The
    // selector builds its pop-up from these when it opens.
    bool instrument = jackdaw_track_is_instrument(m_track);
    const char **ports =
        instrument ? jackdaw_engine_list_midi_ports() : jackdaw_engine_list_capture_ports();
    m_input->SetChoices(ports, !instrument);
    if (ports)
        jackdaw_engine_free_ports(ports);
}

void TrackStripView::RefreshInputs()
{
    // The connectable-source list (physical JACK ports) or our own capture-port
    // pool changed: rebuild the menu, then re-mark the current selection.
    BuildInputMenu();
    SyncFromTrack();
}

void TrackStripView::SyncFromTrack()
{
    // Setting control values programmatically here does not re-invoke their
    // messages (BButton/BTextControl/KnobView::SetValue are all silent), so no
    // feedback guard is needed.
    if (strcmp(m_name->Text(), jackdaw_track_get_name(m_track)) != 0)
        m_name->SetText(jackdaw_track_get_name(m_track));
    m_arm->SetValue(jackdaw_track_is_armed(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
    m_mute->SetValue(jackdaw_track_is_muted(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
    m_solo->SetValue(jackdaw_track_is_soloed(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
    // Fx is a momentary button (opens the Fx window); tint it blue whenever the
    // track carries an effect chain.
    m_fx->SetForcedActive(jackdaw_track_fx_count(m_track) > 0);
    bool instrument = jackdaw_track_is_instrument(m_track);
    bool stereo = jackdaw_track_is_stereo(m_track);
    m_stereo->SetValue(stereo ? B_CONTROL_ON : B_CONTROL_OFF);
    m_stereo->SetLabel(instrument ? "MIDI" : (stereo ? "St" : "Mo"));
    m_stereo->SetEnabled(!instrument); // mono/stereo is an audio-track concept
    double trim_db = (jackdaw_track_get_trim(m_track) > 0.0001f)
                         ? 20.0 * log10((double)jackdaw_track_get_trim(m_track))
                         : -25.0;
    m_vol->SetValue(trim_db);
    m_pan->SetValue((double)jackdaw_track_get_pan(m_track));

    // Show the current input source (MIDI or audio per track kind).
    if (instrument)
        m_input->SetSelected(m_track->midi_src_port);
    else
        m_input->SetSelected(m_track->audio_src_port, stereo ? m_track->audio_src_port_r : NULL);
}

void TrackStripView::SetPeaks(float l, float r)
{
    if (m_vu)
        m_vu->SetPeaks(l, r);
}

void TrackStripView::ApplyStereo(bool stereo)
{
    jackdaw_engine_set_track_stereo(m_track, stereo);
    m_stereo->SetLabel(stereo ? "St" : "Mo");

    if (stereo) {
        // Auto-pair the right channel with the capture port following the
        // currently-selected left source, if one exists.
        const char *left = m_track->audio_src_port;
        if (left) {
            const char **ports = jackdaw_engine_list_capture_ports();
            if (ports) {
                for (int i = 0; ports[i]; i++) {
                    if (strcmp(ports[i], left) == 0 && ports[i + 1]) {
                        jackdaw_engine_set_audio_source_r(m_track, ports[i + 1]);
                        break;
                    }
                }
                jackdaw_engine_free_ports(ports);
            }
        }
    }
}

void TrackStripView::MouseDown(BPoint where)
{
    int32 buttons = 0;
    int32 modifiers = 0;
    BMessage *msg = Window() ? Window()->CurrentMessage() : NULL;
    if (msg) {
        msg->FindInt32("buttons", &buttons);
        msg->FindInt32("modifiers", &modifiers);
    }

    if (buttons & B_SECONDARY_MOUSE_BUTTON) {
        BPoint screen = ConvertToScreen(where);
        BMessage m(MSG_TRACK_CONTEXT);
        m.AddInt32("slot", (int32)m_track->slot);
        m.AddPoint("screen_where", screen);
        m_main.SendMessage(&m);
        return;
    }

    // Left click selects the track (Shift/Cmd extends the selection).
    if (modifiers & (B_SHIFT_KEY | B_COMMAND_KEY)) {
        jackdaw_project_toggle_selected(m_project, m_track);
        return; // multi-select gesture, not a reorder drag
    }
    jackdaw_project_select_single(m_project, m_track);

    // Prime a drag-to-reorder: a plain press on the strip background (child
    // controls consume their own clicks) can become a row drag once the pointer
    // moves past a small threshold.
    m_maybe_drag = true;
    m_did_drag = false;
    m_drag_start_y = where.y;
    m_drag_from = jackdaw_project_track_index(m_project, m_track);
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
}

// Insertion boundary in [0, track count] for the current pointer position. The
// vertical scroll offset cancels out because the strip sits at (from*rowH -
// scroll), so only the press-time index and the local y are needed.
int TrackStripView::DropGapFor(float local_y) const
{
    int n = (int)jackdaw_project_track_count(m_project);
    float rowH = kTimelineTrackHeight;
    int gap = (int)floorf(((float)m_drag_from * rowH + local_y) / rowH + 0.5f);
    if (gap < 0)
        gap = 0;
    if (gap > n)
        gap = n;
    return gap;
}

void TrackStripView::MouseMoved(BPoint where, uint32 code, const BMessage *drag)
{
    (void)code;
    (void)drag;
    if (!m_maybe_drag)
        return;
    if (!m_did_drag && fabsf(where.y - m_drag_start_y) < 6.0f)
        return; // below the movement threshold: still a click, not a drag
    m_did_drag = true;
    if (TrackAreaView *area = dynamic_cast<TrackAreaView *>(Parent()))
        area->SetDropGap(DropGapFor(where.y));
}

void TrackStripView::MouseUp(BPoint where)
{
    if (!m_maybe_drag)
        return;
    bool did = m_did_drag;
    m_maybe_drag = false;
    m_did_drag = false;
    if (TrackAreaView *area = dynamic_cast<TrackAreaView *>(Parent()))
        area->SetDropGap(-1);
    if (!did || m_drag_from < 0)
        return;

    int n = (int)jackdaw_project_track_count(m_project);
    int gap = DropGapFor(where.y);
    // Removing the source first shifts everything after it up by one, so a gap
    // below the source maps to gap-1 in the post-removal array.
    int to = (gap > m_drag_from) ? gap - 1 : gap;
    if (to < 0)
        to = 0;
    if (to > n - 1)
        to = n - 1;
    if (to == m_drag_from)
        return;
    BMessage m(MSG_TRACK_MOVE);
    m.AddInt32("from", m_drag_from);
    m.AddInt32("to", to);
    m_main.SendMessage(&m);
}

void TrackStripView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect b = Bounds();
    // The opaque view colour has already erased the background. Just draw the
    // selection accent bar and the row dividers on top.
    if (jackdaw_project_is_selected(m_project, m_track)) {
        SetHighColor(60, 150, 230);
        FillRect(BRect(b.left, b.top, b.left + 2.0f, b.bottom));
    }
    SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
    StrokeLine(BPoint(b.right, b.top), BPoint(b.right, b.bottom));
    StrokeLine(BPoint(b.left, b.bottom), BPoint(b.right, b.bottom));
}

void TrackStripView::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_S_NAME:
            jackdaw_track_set_name(m_track, m_name->Text());
            break;
        case MSG_S_ARM:
            jackdaw_track_set_armed(m_track, m_arm->Value() == B_CONTROL_ON);
            break;
        case MSG_S_MUTE:
            jackdaw_track_set_muted(m_track, m_mute->Value() == B_CONTROL_ON);
            break;
        case MSG_S_SOLO:
            jackdaw_track_set_soloed(m_track, m_solo->Value() == B_CONTROL_ON);
            break;
        case MSG_S_STEREO:
            ApplyStereo(m_stereo->Value() == B_CONTROL_ON);
            break;
        case MSG_S_VOL: {
            float db = 0.0f;
            message->FindFloat("value", &db);
            jackdaw_track_set_trim(m_track, (gfloat)pow(10.0, (double)db / 20.0));
            break;
        }
        case MSG_S_PAN: {
            float pan = 0.0f;
            message->FindFloat("value", &pan);
            jackdaw_track_set_pan(m_track, (gfloat)pan);
            break;
        }
        case MSG_S_FX: {
            // FX windows are owned by the main window (one per track, shared with
            // the mixer's Fx button); ask it to open/raise this track's window.
            BMessage m(MSG_OPEN_FX);
            m.AddPointer("track", m_track);
            m_main.SendMessage(&m);
            break;
        }
        case MSG_S_INPUT: {
            const char *port = "";
            message->FindString("port", &port);
            const char *sel = (port && *port) ? port : NULL;
            if (jackdaw_track_is_instrument(m_track)) {
                jackdaw_engine_set_midi_source(m_track, sel);
            } else {
                const char *right = NULL;
                if (sel && message->FindString("port_r", &right) == B_OK && right && *right) {
                    // A stereo pair: the track becomes stereo, the pair's
                    // channels feed its left and right.
                    jackdaw_engine_set_track_stereo(m_track, TRUE);
                    jackdaw_engine_set_audio_source_l(m_track, sel);
                    jackdaw_engine_set_audio_source_r(m_track, right);
                } else {
                    jackdaw_engine_set_audio_source_l(m_track, sel);
                    if (jackdaw_track_is_stereo(m_track)) {
                        if (sel)
                            ApplyStereo(true); // re-pair the right channel to the new source
                        else
                            jackdaw_engine_set_audio_source_r(m_track, NULL); // None: both
                    }
                }
            }
            SyncFromTrack();
            break;
        }
        default:
            BView::MessageReceived(message);
            break;
    }
}
