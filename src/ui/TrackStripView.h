#pragma once

#include <Messenger.h>
#include <View.h>

#include "engine/project.h"
#include "engine/track.h"

class BButton;
class BTextControl;
class KnobView;
class PortSelector;
class StateButton;
class VuView;

// One track's header strip (the 235 px column left of its timeline lane): name
// entry; Arm/Mute/Solo/Mono-Stereo/Fx buttons (Fx opens the track's VST3 FX
// chain window); trim (V) and pan (P) knobs; a JACK capture-port input
// selector (fixed width, the name shortened to fit); and a stereo VU. It lives on the MainWindow looper, so it calls
// the track / engine API directly; structural actions (select, delete,
// context menu) go to MainWindow. SyncFromTrack() re-reads engine state.
class TrackStripView : public BView
{
public:
    TrackStripView(JackDawProject *project, JackDawTrack *track, const BMessenger &main);

    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *drag) override;
    void MessageReceived(BMessage *message) override;

    JackDawTrack *Track() const
    {
        return m_track;
    }
    void SetPeaks(float l, float r); // called on the UI tick
    void SyncFromTrack();            // refresh controls from track/engine state
    void RefreshInputs();            // rebuild the input menu + re-mark (ports changed)

private:
    void BuildInputMenu();
    void ApplyStereo(bool stereo);
    int DropGapFor(float local_y) const; // reorder insertion boundary for a pointer y

    JackDawProject *m_project; // borrowed
    JackDawTrack *m_track;     // borrowed (project owns the ref)
    BMessenger m_main;

    BTextControl *m_name;
    StateButton *m_arm;
    StateButton *m_mute;
    StateButton *m_solo;
    BButton *m_stereo; // Mo <-> St
    StateButton *m_fx; // opens this track's FxWindow (blue when it carries a chain)
    KnobView *m_vol;   // trim (V)
    KnobView *m_pan;   // pan (P)
    PortSelector *m_input;
    VuView *m_vu;

    gulong m_state_handler;   // track "state-changed" -> SyncFromTrack
    gulong m_routing_handler; // track "routing-changed" -> SyncFromTrack

    // Drag-to-reorder gesture state (a Haiku-port addition; the Linux UI has no
    // track-reorder drag). A left-press primes a drag; once the pointer moves
    // past a threshold the row can be dropped at a new position.
    bool m_maybe_drag;
    bool m_did_drag;
    float m_drag_start_y; // strip-local y at press
    int m_drag_from;      // track's array index at press
};
