#ifndef MIDICONTROL_H_INCLUDED
#define MIDICONTROL_H_INCLUDED

#include <glib.h>

#include "project.h"

G_BEGIN_DECLS

/*
 * MIDI control surface — maps incoming MIDI messages from a dedicated control
 * input (e.g. a guitar footswitch on jackdaw:control_in) to actions: FX bypass,
 * wet/dry mix, plugin parameters, track mute, gapless track switching, and
 * transport. Modeled on the Reaper/Ardour "generic MIDI" approach: a binding
 * matches a message by {type, channel, number} and is populated via MIDI learn.
 *
 * Threading: the engine reads control_in on the JACK RT thread and pushes raw
 * bytes into a ringbuffer; the main thread drains it (jackdaw_engine_control_poll)
 * and calls midicontrol_dispatch_event(). EVERYTHING in this module runs on the
 * main thread only — the mapping table is plain (no atomics, never touched by RT).
 */

typedef enum {
    MIDI_CTL_CC = 0,    /* control change  (0xB0) */
    MIDI_CTL_NOTE,      /* note on/off     (0x90 / 0x80) */
    MIDI_CTL_PROGRAM,   /* program change  (0xC0) */
    MIDI_CTL_PITCHBEND, /* pitch bend      (0xE0) */
    MIDI_CTL_NTYPES
} MidiCtlMsgType;

/* msg_type sentinel for a mapping whose trigger has not been learned yet. */
#define MIDI_CTL_UNLEARNED 0xFF

typedef enum {
    ACT_TOGGLE_BYPASS = 0, /* press flips the plugin's enabled/bypass state */
    ACT_MOMENTARY_BYPASS,  /* plugin enabled while held (CC>=64 / note-on) */
    ACT_SET_MIX,           /* CC value 0..127 -> wet/dry 0..1 */
    ACT_SET_PARAM,         /* CC value 0..127 -> plugin parameter (normalized) */
    ACT_TOGGLE_MUTE,       /* press flips one track's mute */
    ACT_SWITCH_GROUP,      /* pedalboard: press unmutes my track and mutes the
                            * group's others; pressing it again mutes it */
    ACT_TRANSPORT_PLAY,    /* press toggles play/stop */
    ACT_TRANSPORT_STOP,    /* press stops */
    ACT_TRANSPORT_REC,     /* press toggles record */
    ACT_NACTIONS
} MidiCtlAction;

typedef struct {
    /* match */
    guint8 msg_type; /* MidiCtlMsgType, or MIDI_CTL_UNLEARNED */
    gint8 channel;   /* 0..15, or -1 = any channel */
    guint8 number;   /* CC# or note# (0..127); ignored for pitch bend */
    /* action + target */
    guint8 action;       /* MidiCtlAction */
    gint32 track_index;  /* index into project tracks, or -1 */
    gint32 fx_index;     /* FX slot on that track, or -1 */
    gint32 param_index;  /* ACT_SET_PARAM only, else -1 */
    gint32 switch_group; /* ACT_SWITCH_GROUP group id, or -1 */
} MidiCtlMapping;

/* Transport hook: `which` is ACT_TRANSPORT_PLAY / _STOP / _REC. Registered by
 * the main window so transport actions drive the toolbar buttons (keeping the
 * UI in sync) rather than calling the engine blindly. */
typedef void (*MidiCtlTransportFn)(int which, gpointer user);
/* Fired after MIDI learn captures a binding, so an open window can refresh. */
typedef void (*MidiCtlChangedFn)(gpointer user);
/* Fired after a mapping changes an effect's bypass or wet/dry on track `t`, so an
 * open FX window can show the new state. */
typedef void (*MidiCtlFxFn)(JackDawTrack *t, gpointer user);

void midicontrol_init(void);
void midicontrol_shutdown(void);

guint midicontrol_count(void);
MidiCtlMapping *midicontrol_get(guint i);       /* borrowed; NULL if out of range */
guint midicontrol_add(const MidiCtlMapping *m); /* m=NULL => blank */
void midicontrol_remove(guint i);
void midicontrol_clear(void);

/* Arm a mapping slot for MIDI learn (-1 = off). The next "press" event captures
 * that mapping's {type, channel, number}. */
void midicontrol_set_learn(gint mapping_index);
gint midicontrol_get_learn(void);

/* Main thread: interpret one control-surface MIDI event (raw bytes). */
void midicontrol_dispatch_event(JackDawProject *p, const guint8 *data, guint8 size);

void midicontrol_set_transport_cb(MidiCtlTransportFn fn, gpointer user);
void midicontrol_set_changed_cb(MidiCtlChangedFn fn, gpointer user);
void midicontrol_set_fx_cb(MidiCtlFxFn fn, gpointer user);

/* Per-project persistence (mappings reference track/FX indices). */
void midicontrol_save_keyfile(GKeyFile *kf);
void midicontrol_load_keyfile(GKeyFile *kf);

G_END_DECLS

#endif /* MIDICONTROL_H_INCLUDED */
