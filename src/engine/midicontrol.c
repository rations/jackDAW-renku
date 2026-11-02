#include <string.h>

#include "host/pluginhost.h"
#include "midicontrol.h"
#include "track.h"

/* All state is main-thread only (see midicontrol.h). */
static GArray *mappings; /* of MidiCtlMapping */
static gint g_learn_index = -1;
static MidiCtlTransportFn g_transport_cb;
static gpointer g_transport_user;
static MidiCtlChangedFn g_changed_cb;
static gpointer g_changed_user;
static MidiCtlFxFn g_fx_cb;
static gpointer g_fx_user;

/* ---- Lifecycle / table management ------------------------------------- */

void midicontrol_init(void)
{
    if (!mappings)
        mappings = g_array_new(FALSE, TRUE, sizeof(MidiCtlMapping));
    g_learn_index = -1;
}

void midicontrol_shutdown(void)
{
    if (mappings) {
        g_array_free(mappings, TRUE);
        mappings = NULL;
    }
    g_learn_index = -1;
}

guint midicontrol_count(void)
{
    return mappings ? mappings->len : 0;
}

MidiCtlMapping *midicontrol_get(guint i)
{
    if (!mappings || i >= mappings->len)
        return NULL;
    return &g_array_index(mappings, MidiCtlMapping, i);
}

guint midicontrol_add(const MidiCtlMapping *m)
{
    MidiCtlMapping mm;
    if (m) {
        mm = *m;
    } else {
        memset(&mm, 0, sizeof mm);
        mm.msg_type = MIDI_CTL_UNLEARNED;
        mm.channel = -1;
        mm.action = ACT_TOGGLE_BYPASS;
        mm.track_index = -1;
        mm.fx_index = -1;
        mm.param_index = -1;
        mm.switch_group = -1;
    }
    if (!mappings)
        midicontrol_init();
    g_array_append_val(mappings, mm);
    return mappings->len - 1;
}

void midicontrol_remove(guint i)
{
    if (mappings && i < mappings->len)
        g_array_remove_index(mappings, i);
    if (g_learn_index == (gint)i)
        g_learn_index = -1;
}

void midicontrol_clear(void)
{
    if (mappings)
        g_array_set_size(mappings, 0);
    g_learn_index = -1;
}

void midicontrol_set_learn(gint i)
{
    g_learn_index = i;
}
gint midicontrol_get_learn(void)
{
    return g_learn_index;
}

void midicontrol_set_transport_cb(MidiCtlTransportFn fn, gpointer u)
{
    g_transport_cb = fn;
    g_transport_user = u;
}

void midicontrol_set_changed_cb(MidiCtlChangedFn fn, gpointer u)
{
    g_changed_cb = fn;
    g_changed_user = u;
}

void midicontrol_set_fx_cb(MidiCtlFxFn fn, gpointer u)
{
    g_fx_cb = fn;
    g_fx_user = u;
}

/* ---- Event parsing ----------------------------------------------------- */

typedef struct {
    int type;       /* MidiCtlMsgType */
    int channel;    /* 0..15 */
    int number;     /* CC#/note#; 0 for pitch bend */
    int value;      /* 0..127 (velocity for notes, MSB for pitch bend) */
    gboolean press; /* leading edge: note-on / CC!=0 / pgm / bend */
} CtlEvent;

/* Decode a raw 1..3 byte MIDI message into a CtlEvent. Returns FALSE for
 * messages we don't map (system messages, running status, aftertouch). */
static gboolean parse_event(const guint8 *d, guint8 size, CtlEvent *e)
{
    if (size < 1)
        return FALSE;
    int st = d[0] & 0xF0;
    e->channel = d[0] & 0x0F;
    e->number = 0;
    e->value = 0;

    switch (st) {
        case 0x90: /* note on (velocity 0 == note off) */
            if (size < 3)
                return FALSE;
            e->type = MIDI_CTL_NOTE;
            e->number = d[1] & 0x7F;
            e->value = d[2] & 0x7F;
            e->press = (e->value > 0);
            return TRUE;
        case 0x80: /* note off */
            if (size < 2)
                return FALSE;
            e->type = MIDI_CTL_NOTE;
            e->number = d[1] & 0x7F;
            e->value = 0;
            e->press = FALSE;
            return TRUE;
        case 0xB0: /* control change */
            if (size < 3)
                return FALSE;
            e->type = MIDI_CTL_CC;
            e->number = d[1] & 0x7F;
            e->value = d[2] & 0x7F;
            e->press = (e->value != 0);
            return TRUE;
        case 0xC0: /* program change */
            if (size < 2)
                return FALSE;
            e->type = MIDI_CTL_PROGRAM;
            e->number = d[1] & 0x7F;
            e->press = TRUE;
            return TRUE;
        case 0xE0: /* pitch bend (use MSB as a 0..127 absolute value) */
            if (size < 3)
                return FALSE;
            e->type = MIDI_CTL_PITCHBEND;
            e->value = d[2] & 0x7F;
            e->press = TRUE;
            return TRUE;
        default:
            return FALSE;
    }
}

/* ---- Target resolution (validates against live counts) ----------------- */

static JackDawTrack *resolve_track(JackDawProject *p, gint idx)
{
    if (idx < 0 || (guint)idx >= jackdaw_project_track_count(p))
        return NULL;
    return jackdaw_project_get_track(p, (guint)idx);
}

static PluginInstance *resolve_inst(JackDawProject *p, const MidiCtlMapping *m)
{
    JackDawTrack *t = resolve_track(p, m->track_index);
    if (!t)
        return NULL;
    if (m->fx_index < 0 || (guint)m->fx_index >= jackdaw_track_fx_count(t))
        return NULL;
    return (PluginInstance *)jackdaw_track_fx_get(t, (guint)m->fx_index);
}

/* Tell an open FX window that a mapping changed bypass or wet/dry. Without it
 * the window's enable checkbox and Dry/Wet slider kept showing the old state. */
static void notify_fx(JackDawProject *p, const MidiCtlMapping *m)
{
    if (g_fx_cb)
        g_fx_cb(resolve_track(p, m->track_index), g_fx_user);
}

/* ---- Dispatch ---------------------------------------------------------- */

void midicontrol_dispatch_event(JackDawProject *p, const guint8 *data, guint8 size)
{
    CtlEvent e;
    if (!p || !mappings || !parse_event(data, size, &e))
        return;

    /* MIDI learn: capture this trigger into the armed mapping. Only on a press
     * edge, so a footswitch release (note-off / CC 0) doesn't bind a spurious
     * value — matches Reaper/Ardour learn behaviour. */
    if (g_learn_index >= 0) {
        if (!e.press)
            return;
        if ((guint)g_learn_index < mappings->len) {
            MidiCtlMapping *m = &g_array_index(mappings, MidiCtlMapping, g_learn_index);
            m->msg_type = (guint8)e.type;
            m->channel = (gint8)e.channel;
            m->number = (guint8)e.number;
        }
        g_learn_index = -1;
        if (g_changed_cb)
            g_changed_cb(g_changed_user);
        return;
    }

    for (guint i = 0; i < mappings->len; i++) {
        MidiCtlMapping *m = &g_array_index(mappings, MidiCtlMapping, i);
        if (m->msg_type != (guint8)e.type)
            continue;
        if (m->channel >= 0 && m->channel != e.channel)
            continue;
        if (e.type != MIDI_CTL_PITCHBEND && m->number != (guint8)e.number)
            continue;

        switch (m->action) {
            case ACT_TOGGLE_BYPASS: {
                if (!e.press)
                    break;
                PluginInstance *inst = resolve_inst(p, m);
                if (!inst)
                    break;
                pluginhost_set_active(inst, !pluginhost_is_active(inst));
                notify_fx(p, m);
                break;
            }
            case ACT_MOMENTARY_BYPASS: {
                PluginInstance *inst = resolve_inst(p, m);
                if (!inst)
                    break;
                gboolean on = (e.type == MIDI_CTL_CC) ? (e.value >= 64) : e.press;
                pluginhost_set_active(inst, on);
                notify_fx(p, m);
                break;
            }
            case ACT_SET_MIX: {
                PluginInstance *inst = resolve_inst(p, m);
                if (!inst)
                    break;
                pluginhost_set_mix(inst, (float)e.value / 127.0f);
                notify_fx(p, m);
                break;
            }
            case ACT_SET_PARAM: {
                PluginInstance *inst = resolve_inst(p, m);
                if (!inst)
                    break;
                if (m->param_index < 0 || (guint)m->param_index >= pluginhost_param_count(inst))
                    break;
                /* This port's plugin parameters are VST3-normalized [0,1], so a
                 * CC maps straight onto the normalized value (no min/max query,
                 * unlike the Linux host's pluginhost_param_range). */
                pluginhost_param_set(inst, (guint)m->param_index, (float)e.value / 127.0f);
                break;
            }
            case ACT_TOGGLE_MUTE: {
                if (!e.press)
                    break;
                JackDawTrack *t = resolve_track(p, m->track_index);
                if (t)
                    jackdaw_track_set_muted(t, !jackdaw_track_is_muted(t));
                break;
            }
            case ACT_SWITCH_GROUP: {
                if (!e.press)
                    break;
                JackDawTrack *t = resolve_track(p, m->track_index);
                if (!t)
                    break;
                /* Like a stomp box: pressing the pedal that is on turns it off.
                 * "On" means it is the group's only unmuted track -- in a fresh
                 * project every track is unmuted, and the first press must select
                 * its pedal, not mute it. Tracks outside the group (backing,
                 * drums) are never touched. */
                if (!jackdaw_track_is_muted(t)) {
                    gboolean others_on = FALSE;
                    for (guint j = 0; j < mappings->len && !others_on; j++) {
                        MidiCtlMapping *om = &g_array_index(mappings, MidiCtlMapping, j);
                        if (om->action != ACT_SWITCH_GROUP)
                            continue;
                        if (om->switch_group != m->switch_group)
                            continue;
                        if (om->track_index == m->track_index)
                            continue;
                        JackDawTrack *ot = resolve_track(p, om->track_index);
                        if (ot && !jackdaw_track_is_muted(ot))
                            others_on = TRUE;
                    }
                    if (!others_on) {
                        jackdaw_track_set_muted(t, TRUE);
                        break;
                    }
                }
                /* Unmute the new pedal BEFORE muting the rest: the RT workers read
                 * each track's flag independently, so this order can at worst give
                 * one period with both pedals sounding, never one with neither. */
                jackdaw_track_set_muted(t, FALSE);
                /* Mute every other track that belongs to the same switch group. */
                for (guint j = 0; j < mappings->len; j++) {
                    MidiCtlMapping *om = &g_array_index(mappings, MidiCtlMapping, j);
                    if (om == m)
                        continue;
                    if (om->action != ACT_SWITCH_GROUP)
                        continue;
                    if (om->switch_group != m->switch_group)
                        continue;
                    if (om->track_index == m->track_index)
                        continue;
                    JackDawTrack *ot = resolve_track(p, om->track_index);
                    if (ot)
                        jackdaw_track_set_muted(ot, TRUE);
                }
                break;
            }
            case ACT_TRANSPORT_PLAY:
            case ACT_TRANSPORT_STOP:
            case ACT_TRANSPORT_REC:
                if (e.press && g_transport_cb)
                    g_transport_cb((int)m->action, g_transport_user);
                break;
            default:
                break;
        }
    }
}

/* ---- Persistence (per-project) ----------------------------------------- */

void midicontrol_save_keyfile(GKeyFile *kf)
{
    guint n = mappings ? mappings->len : 0;
    g_key_file_set_integer(kf, "midicontrol", "count", (gint)n);
    for (guint i = 0; i < n; i++) {
        MidiCtlMapping *m = &g_array_index(mappings, MidiCtlMapping, i);
        char grp[32];
        g_snprintf(grp, sizeof grp, "midicontrol%u", i);
        g_key_file_set_integer(kf, grp, "msg_type", m->msg_type);
        g_key_file_set_integer(kf, grp, "channel", m->channel);
        g_key_file_set_integer(kf, grp, "number", m->number);
        g_key_file_set_integer(kf, grp, "action", m->action);
        g_key_file_set_integer(kf, grp, "track", m->track_index);
        g_key_file_set_integer(kf, grp, "fx", m->fx_index);
        g_key_file_set_integer(kf, grp, "param", m->param_index);
        g_key_file_set_integer(kf, grp, "group", m->switch_group);
    }
}

void midicontrol_load_keyfile(GKeyFile *kf)
{
    midicontrol_clear();
    if (!mappings)
        midicontrol_init();
    if (!g_key_file_has_group(kf, "midicontrol"))
        return;

    gint n = g_key_file_get_integer(kf, "midicontrol", "count", NULL);
    if (n < 0)
        n = 0;
    if (n > 256)
        n = 256; /* sanity bound, per the "validate project data" rule */

    for (gint i = 0; i < n; i++) {
        char grp[32];
        g_snprintf(grp, sizeof grp, "midicontrol%d", i);
        if (!g_key_file_has_group(kf, grp))
            continue;

        gint msg = g_key_file_get_integer(kf, grp, "msg_type", NULL);
        gint ch = g_key_file_get_integer(kf, grp, "channel", NULL);
        gint num = g_key_file_get_integer(kf, grp, "number", NULL);
        gint act = g_key_file_get_integer(kf, grp, "action", NULL);
        gint tr = g_key_file_get_integer(kf, grp, "track", NULL);
        gint fx = g_key_file_get_integer(kf, grp, "fx", NULL);
        gint par = g_key_file_get_integer(kf, grp, "param", NULL);
        gint grp_id = g_key_file_get_integer(kf, grp, "group", NULL);

        /* Validate every field before it becomes an array index. */
        if (msg != MIDI_CTL_UNLEARNED && (msg < 0 || msg >= MIDI_CTL_NTYPES))
            continue;
        if (act < 0 || act >= ACT_NACTIONS)
            continue;
        if (ch < -1 || ch > 15)
            ch = -1;
        if (num < 0 || num > 127)
            num = 0;
        if (tr < -1 || tr > 63)
            tr = -1;
        if (fx < -1 || fx > 63)
            fx = -1;
        if (par < -1 || par > 4095)
            par = -1;
        if (grp_id < -1 || grp_id > 63)
            grp_id = -1;

        MidiCtlMapping m;
        memset(&m, 0, sizeof m);
        m.msg_type = (guint8)msg;
        m.channel = (gint8)ch;
        m.number = (guint8)num;
        m.action = (guint8)act;
        m.track_index = tr;
        m.fx_index = fx;
        m.param_index = par;
        m.switch_group = grp_id;
        g_array_append_val(mappings, m);
    }
}
