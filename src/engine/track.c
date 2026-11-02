#include <string.h>
#include "track.h"
#include "host/pluginhost.h"
#include "jackdaw-engine.h"
#include "message.h"

G_DEFINE_TYPE(JackDawTrack, jackdaw_track, G_TYPE_OBJECT)

enum { SIGNAL_STATE_CHANGED, SIGNAL_ROUTING_CHANGED, LAST_SIGNAL };

static guint track_signals[LAST_SIGNAL];

/* ---- GObject boilerplate ---- */

static void jackdaw_track_finalize(GObject *obj)
{
    JackDawTrack *t = JACKDAW_TRACK(obj);

    g_free(t->name);
    g_free(t->audio_src_port);
    g_free(t->audio_src_port_r);
    g_free(t->midi_src_port);

    if (t->regions)
        g_ptr_array_unref(t->regions);
    if (t->rt_snapshot)
        clip_region_snapshot_unref(t->rt_snapshot);
    g_mutex_clear(&t->region_lock);

    if (t->play_buf_L)
        rt_ringbuffer_free(t->play_buf_L);
    if (t->play_buf_R)
        rt_ringbuffer_free(t->play_buf_R);
    if (t->rec_buf_L)
        rt_ringbuffer_free(t->rec_buf_L);
    if (t->rec_buf_R)
        rt_ringbuffer_free(t->rec_buf_R);
    if (t->midi_rec_buf)
        rt_ringbuffer_free(t->midi_rec_buf);
    g_free((gpointer)t->rec_peak_buf);

    /* MIDI: drop the live snapshot + any retired ones, then the regions/clip. */
    if (t->rt_midi)
        midi_event_snapshot_free((MidiEventSnapshot *)t->rt_midi);
    if (t->retire_midi) {
        for (guint i = 0; i < t->retire_midi->len; i++)
            midi_event_snapshot_free(g_ptr_array_index(t->retire_midi, i));
        g_ptr_array_free(t->retire_midi, TRUE);
    }
    if (t->midi_regions)
        g_ptr_array_unref(t->midi_regions);
    if (t->midi_clip)
        midi_clip_free(t->midi_clip);

    /* FX: the engine slot was cleared before the track died, so the RT thread
     * no longer reads rt_chain — free the live chain, everything retired, and
     * the instances themselves. */
    JackDawFxChain *live = t->rt_chain;
    t->rt_chain = NULL;
    if (live) {
        g_free(live->fx);
        g_free(live);
    }
    if (t->retire_chains) {
        for (guint i = 0; i < t->retire_chains->len; i++) {
            JackDawFxChain *c = g_ptr_array_index(t->retire_chains, i);
            g_free(c->fx);
            g_free(c);
        }
        g_ptr_array_free(t->retire_chains, TRUE);
    }
    if (t->retire_fx) {
        for (guint i = 0; i < t->retire_fx->len; i++)
            pluginhost_free(g_ptr_array_index(t->retire_fx, i));
        g_ptr_array_free(t->retire_fx, TRUE);
    }
    if (t->fx_list) {
        for (guint i = 0; i < t->fx_list->len; i++)
            pluginhost_free(g_ptr_array_index(t->fx_list, i));
        g_ptr_array_free(t->fx_list, TRUE);
    }

    G_OBJECT_CLASS(jackdaw_track_parent_class)->finalize(obj);
}

static void jackdaw_track_class_init(JackDawTrackClass *klass)
{
    GObjectClass *gc = G_OBJECT_CLASS(klass);
    gc->finalize = jackdaw_track_finalize;

    track_signals[SIGNAL_STATE_CHANGED] = g_signal_new(
        "state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawTrackClass, state_changed), NULL, NULL, NULL, G_TYPE_NONE, 0);

    track_signals[SIGNAL_ROUTING_CHANGED] = g_signal_new(
        "routing-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawTrackClass, routing_changed), NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void jackdaw_track_init(JackDawTrack *t)
{
    t->name = NULL;
    t->slot = G_MAXUINT;
    t->kind = JACKDAW_TRACK_AUDIO;
    t->audio_in_idx = -1;
    t->midi_in_idx = -1;
    t->audio_src_port = NULL;
    t->audio_src_port_r = NULL;
    t->midi_src_port = NULL;
    t->stereo_input = FALSE;
    t->state_flags = 0;
    t->volume = 1.0f;
    t->trim = 1.0f;
    t->fader = 1.0f;
    t->pan = 0.0f;
    t->peak_L = 0.0f;
    t->peak_R = 0.0f;

    t->regions = clip_region_list_new();
    g_mutex_init(&t->region_lock);
    t->rt_snapshot = clip_region_snapshot_new(t->regions);
    t->clip_start = 0;
    t->play_buf_L = NULL;
    t->play_buf_R = NULL;
    t->rec_buf_L = NULL;
    t->rec_buf_R = NULL;
    t->midi_rec_buf = NULL;

    /* MIDI model: an oversized default clip (4 bars * 1000, in ticks) so freshly
     * recorded / added notes always fit; regions window into it; snapshot lazy. */
    t->midi_clip = midi_clip_new(JACKDAW_PPQ * 4 * 1000);
    t->midi_regions = midi_region_list_new();
    t->rt_midi = NULL;
    t->retire_midi = g_ptr_array_new();

    t->rec_start_frame = 0;
    t->rec_latency = 0;
    t->rec_peak_buf = NULL;
    t->rec_peak_count = 0;
    t->rec_peak_block = 0;
    t->played_frames = 0;

    t->fx_list = g_ptr_array_new();
    t->rt_chain = NULL;
    t->retire_chains = g_ptr_array_new();
    t->retire_fx = g_ptr_array_new();
}

/* ---- Constructor ---- */

JackDawTrack *jackdaw_track_new(const gchar *name, AudioClip *clip)
{
    JackDawTrack *t = g_object_new(JACKDAW_TYPE_TRACK, NULL);

    t->name = g_strdup(name ? name : "Track");

    /* If an initial clip is supplied, place it as a single region at tl=0.
     * jackdaw_track_set_clip consumes one reference, matching the take-ownership
     * contract of this constructor. Audio ringbuffers are allocated later by
     * jackdaw_engine_add_track() once the JACK sample rate is known. */
    if (clip)
        jackdaw_track_set_clip(t, clip);

    return t;
}

/* ---- Clip regions ---- */

/* Region duration on the timeline (timeline frames) for a whole file. */
static off_t clip_timeline_length(AudioClip *clip)
{
    if (!clip)
        return 0;
    int jack_sr = (int)jackdaw_engine_get_sample_rate();
    int clip_sr = clip->info.samplerate;
    if (clip_sr <= 0 || jack_sr <= 0 || clip_sr == jack_sr)
        return clip->info.frames;
    return (off_t)((double)clip->info.frames * (double)jack_sr / (double)clip_sr + 0.5);
}

AudioClip *jackdaw_track_get_clip(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    if (!t->regions || t->regions->len == 0)
        return NULL;
    ClipRegion *r = g_ptr_array_index(t->regions, 0);
    return r->clip;
}

void jackdaw_track_place_clip(JackDawTrack *t, AudioClip *clip, off_t tl_pos)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (!clip)
        return;
    ClipRegion *r = clip_region_new(clip, 0, clip_timeline_length(clip), tl_pos);
    g_ptr_array_add(t->regions, r);
    clip_region_list_sort(t->regions);
    audio_clip_free(clip); /* consume caller's reference */
    jackdaw_track_commit_regions(t);
}

void jackdaw_track_set_clip(JackDawTrack *t, AudioClip *new_clip)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (t->regions->len > 0)
        g_ptr_array_remove_range(t->regions, 0, t->regions->len);
    if (new_clip)
        jackdaw_track_place_clip(t, new_clip, t->clip_start);
    else
        jackdaw_track_commit_regions(t);
}

GPtrArray *jackdaw_track_get_regions(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->regions;
}

void jackdaw_track_commit_regions(JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    clip_region_list_sort(t->regions);
    ClipRegionSnapshot *snap = clip_region_snapshot_new(t->regions);
    g_mutex_lock(&t->region_lock);
    ClipRegionSnapshot *old = t->rt_snapshot;
    t->rt_snapshot = snap;
    g_mutex_unlock(&t->region_lock);
    if (old)
        clip_region_snapshot_unref(old);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_apply_regions(JackDawTrack *t, GPtrArray *list)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (t->regions->len > 0)
        g_ptr_array_remove_range(t->regions, 0, t->regions->len);
    if (list) {
        for (guint i = 0; i < list->len; i++)
            g_ptr_array_add(t->regions, clip_region_copy(g_ptr_array_index(list, i)));
    }
    jackdaw_track_commit_regions(t);
}

off_t jackdaw_track_total_frames(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 0);
    return clip_region_list_total_frames(t->regions);
}

ClipRegionSnapshot *jackdaw_track_ref_snapshot(JackDawTrack *t)
{
    ClipRegionSnapshot *s;
    g_mutex_lock(&t->region_lock);
    s = clip_region_snapshot_ref(t->rt_snapshot);
    g_mutex_unlock(&t->region_lock);
    return s;
}

/* ---- Accessors ---- */

const gchar *jackdaw_track_get_name(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->name;
}

void jackdaw_track_set_name(JackDawTrack *t, const gchar *name)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    g_free(t->name);
    t->name = g_strdup(name ? name : "Track");
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

/* ---- Track kind ---- */

JackDawTrackKind jackdaw_track_get_kind(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), JACKDAW_TRACK_AUDIO);
    return t->kind;
}

static guint32 midi_content_end_ticks(MidiClip *c);

/* Seed the default full-clip timeline region if the track has none, so the whole
 * clip is grabbable/splittable. Not called from commit — a track legitimately
 * emptied by a cross-track move must stay empty rather than resurrect a region. */
void jackdaw_track_ensure_midi_region(JackDawTrack *t)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (t->midi_regions->len > 0)
        return;
    MidiRegion *r = midi_region_new(t->midi_clip, 0, midi_content_end_ticks(t->midi_clip), 0);
    r->auto_grow = TRUE; /* the full-clip default lane */
    g_ptr_array_add(t->midi_regions, r);
}

void jackdaw_track_set_kind(JackDawTrack *t, JackDawTrackKind kind)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->kind = kind;
    if (kind == JACKDAW_TRACK_INSTRUMENT)
        jackdaw_track_ensure_midi_region(t);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean jackdaw_track_is_instrument(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), FALSE);
    return t->kind == JACKDAW_TRACK_INSTRUMENT;
}

MidiClip *jackdaw_track_get_midi_clip(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->midi_clip;
}

GPtrArray *jackdaw_track_get_midi_regions(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->midi_regions;
}

/* Tick position one past the last note (and at least the clip's nominal length),
 * i.e. how far a single full-clip region must extend to cover all content. */
static guint32 midi_content_end_ticks(MidiClip *c)
{
    guint32 end = c ? c->length : 0;
    guint n = midi_clip_note_count(c);
    for (guint i = 0; i < n; i++) {
        MidiNote *nt = midi_clip_note(c, i);
        guint32 e = nt->start + nt->length;
        if (e > end)
            end = e;
    }
    return end;
}

/* Keep midi_regions consistent before publishing: grow a lone untouched default
 * region (windowing into this track's own clip) to cover newly added notes, so
 * plain editing/recording keeps everything audible. Regions dragged in from
 * another track keep their own clip ref and length. An empty list stays empty. */
static void track_normalize_midi_regions(JackDawTrack *t)
{
    GPtrArray *regs = t->midi_regions;
    guint32 content = midi_content_end_ticks(t->midi_clip);
    for (guint i = 0; i < regs->len; i++) {
        MidiRegion *r = g_ptr_array_index(regs, i);
        if (r->auto_grow && r->clip == t->midi_clip && r->length < content)
            r->length = content;
    }
}

/* Publish a fresh MIDI event snapshot for the RT thread: reclaim the PREVIOUS
 * edit's retired snapshot (the RT thread has moved past it), build the new one,
 * atomic-swap, retire the old. Mirrors jackdaw_track_commit_regions. */
void jackdaw_track_commit_midi(JackDawTrack *t, double frames_per_beat)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));

    for (guint i = 0; i < t->retire_midi->len; i++)
        midi_event_snapshot_free(g_ptr_array_index(t->retire_midi, i));
    g_ptr_array_set_size(t->retire_midi, 0);

    track_normalize_midi_regions(t);
    MidiEventSnapshot *ns = midi_event_snapshot_new_regions(t->midi_regions, frames_per_beat);
    MidiEventSnapshot *old = t->rt_midi;
    g_atomic_pointer_set(&t->rt_midi, ns);
    if (old)
        g_ptr_array_add(t->retire_midi, old);

    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_set_midi_clip(JackDawTrack *t, MidiClip *clip, double frames_per_beat)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (clip == t->midi_clip) {
        midi_clip_free(clip);
        return;
    }
    MidiClip *old = t->midi_clip;
    t->midi_clip = clip; /* take ownership */
    /* Repoint this track's own regions (those windowing into the replaced clip)
     * onto the new clip so editor undo/redo restores them in place. Regions that
     * reference a different clip (dragged in from another track) are untouched. */
    for (guint i = 0; i < t->midi_regions->len; i++) {
        MidiRegion *r = g_ptr_array_index(t->midi_regions, i);
        if (r->clip == old) {
            r->clip = midi_clip_ref(clip);
            midi_clip_free(old);
        }
    }
    if (old)
        midi_clip_free(old);
    jackdaw_track_commit_midi(t, frames_per_beat);
}

void jackdaw_track_apply_midi_regions(JackDawTrack *t, GPtrArray *list, double frames_per_beat)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (t->midi_regions->len > 0)
        g_ptr_array_remove_range(t->midi_regions, 0, t->midi_regions->len);
    if (list) {
        for (guint i = 0; i < list->len; i++)
            g_ptr_array_add(t->midi_regions, midi_region_copy(g_ptr_array_index(list, i)));
    }
    jackdaw_track_commit_midi(t, frames_per_beat);
}

/* ---- State flags ---- */

void jackdaw_track_set_armed(JackDawTrack *t, gboolean armed)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (armed)
        g_atomic_int_or(&t->state_flags, TRACK_ARMED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_ARMED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_set_muted(JackDawTrack *t, gboolean muted)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (muted)
        g_atomic_int_or(&t->state_flags, TRACK_MUTED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_MUTED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_set_soloed(JackDawTrack *t, gboolean soloed)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (soloed)
        g_atomic_int_or(&t->state_flags, TRACK_SOLOED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_SOLOED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean jackdaw_track_is_armed(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_ARMED) != 0;
}

gboolean jackdaw_track_is_muted(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_MUTED) != 0;
}

gboolean jackdaw_track_is_soloed(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_SOLOED) != 0;
}

/* ---- Volume / pan ---- */

/* Fold the two stages into the effective gain the RT callback reads. */
static void track_recompute_volume(JackDawTrack *t)
{
    gfloat v = t->trim * t->fader;
    t->volume = CLAMP(v, 0.0f, 18.0f); /* 18.0 ≈ linear gain for +25 dB */
}

void jackdaw_track_set_volume(JackDawTrack *t, gfloat vol)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    /* Legacy entry point: treat the value as the fader stage, trim stays unity. */
    t->fader = CLAMP(vol, 0.0f, 18.0f);
    track_recompute_volume(t);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gfloat jackdaw_track_get_volume(JackDawTrack *t)
{
    return t->volume;
}

void jackdaw_track_set_trim(JackDawTrack *t, gfloat trim)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->trim = CLAMP(trim, 0.0f, 18.0f);
    track_recompute_volume(t);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gfloat jackdaw_track_get_trim(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 1.0f);
    return t->trim;
}

void jackdaw_track_set_fader(JackDawTrack *t, gfloat fader)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->fader = CLAMP(fader, 0.0f, 18.0f);
    track_recompute_volume(t);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gfloat jackdaw_track_get_fader(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 1.0f);
    return t->fader;
}

void jackdaw_track_set_pan(JackDawTrack *t, gfloat pan)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->pan = CLAMP(pan, -1.0f, 1.0f);
}

gfloat jackdaw_track_get_pan(JackDawTrack *t)
{
    return t->pan;
}

/* ---- Input routing ---- */

void jackdaw_track_set_audio_in(JackDawTrack *t, gint idx)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->audio_in_idx = idx;
    g_signal_emit(t, track_signals[SIGNAL_ROUTING_CHANGED], 0);
}

void jackdaw_track_set_midi_in(JackDawTrack *t, gint idx)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->midi_in_idx = idx;
    g_signal_emit(t, track_signals[SIGNAL_ROUTING_CHANGED], 0);
}

gboolean jackdaw_track_is_stereo(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), FALSE);
    return t->stereo_input;
}

/* ---- Peak metering ---- */

void jackdaw_track_get_peaks(JackDawTrack *t, gfloat *out_L, gfloat *out_R)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    /* Non-destructive read: the RT callback applies decay-hold, so multiple
     * meters (track strip + mixer) can poll this concurrently. */
    if (out_L)
        *out_L = t->peak_L;
    if (out_R)
        *out_R = t->peak_R;
}

/* ---- FX chain ---- */

/* Free the retired instance queue. Callers must already have established that
 * the RT thread cannot reach them (see track_retire_is_safe / finalize). */
static void track_free_retired_fx(JackDawTrack *t)
{
    for (guint i = 0; i < t->retire_fx->len; i++)
        pluginhost_free(g_ptr_array_index(t->retire_fx, i));
    g_ptr_array_set_size(t->retire_fx, 0);
}

/* TRUE once the RT thread has completed a cycle after the retire — it can no
 * longer be inside the chain snapshot that referenced these instances. Two
 * cycles for margin. With the engine stopped nothing reads the chain at all. */
static gboolean track_retire_is_safe(JackDawTrack *t)
{
    if (!jackdaw_engine_is_processing())
        return TRUE;
    /* Unsigned wrap-around subtraction: correct across the counter's rollover. */
    return (guint)(jackdaw_engine_get_cycle_count() - t->retire_cycle) >= 2u;
}

gboolean jackdaw_track_fx_collect(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (t->retire_fx->len == 0)
        return TRUE;
    if (!track_retire_is_safe(t))
        return FALSE;
    track_free_retired_fx(t);
    return TRUE;
}

/* Reclaim chains/instances retired by the PREVIOUS edit (the RT thread has
 * had many cycles to move past them by now), then publish a fresh chain built
 * from fx_list and retire the old one. */
static void track_publish_chain(JackDawTrack *t)
{
    for (guint i = 0; i < t->retire_chains->len; i++) {
        JackDawFxChain *c = g_ptr_array_index(t->retire_chains, i);
        g_free(c->fx);
        g_free(c);
    }
    g_ptr_array_set_size(t->retire_chains, 0);
    track_free_retired_fx(t);

    JackDawFxChain *nc = g_new0(JackDawFxChain, 1);
    nc->n = (int)t->fx_list->len;
    if (nc->n > 0) {
        nc->fx = g_new0(gpointer, nc->n);
        for (int i = 0; i < nc->n; i++)
            nc->fx[i] = g_ptr_array_index(t->fx_list, i);
    }

    JackDawFxChain *old = t->rt_chain;
    g_atomic_pointer_set(&t->rt_chain, nc);
    if (old)
        g_ptr_array_add(t->retire_chains, old);
}

void jackdaw_track_fx_add(JackDawTrack *t, gpointer instance)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (!instance)
        return;
    g_ptr_array_add(t->fx_list, instance);
    track_publish_chain(t);
}

void jackdaw_track_fx_remove(JackDawTrack *t, guint index)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (index >= t->fx_list->len)
        return;
    gpointer inst = g_ptr_array_index(t->fx_list, index);
    g_ptr_array_remove_index(t->fx_list, index);
    track_publish_chain(t);
    /* Defer the instance free until the RT thread has moved past the chain
     * snapshot that still references it. The caller collects it shortly after
     * (jackdaw_track_fx_collect) rather than waiting for the next edit —
     * a Wine-bridged plug-in holds a Wine host process until it is freed. */
    g_ptr_array_add(t->retire_fx, inst);
    t->retire_cycle = jackdaw_engine_get_cycle_count();
}

void jackdaw_track_fx_move(JackDawTrack *t, guint from, guint to)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (from >= t->fx_list->len || to >= t->fx_list->len || from == to)
        return;
    gpointer inst = g_ptr_array_index(t->fx_list, from);
    g_ptr_array_remove_index(t->fx_list, from);
    g_ptr_array_insert(t->fx_list, (gint)to, inst);
    track_publish_chain(t);
}

guint jackdaw_track_fx_count(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 0);
    return t->fx_list->len;
}

gpointer jackdaw_track_fx_get(JackDawTrack *t, guint index)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return index < t->fx_list->len ? g_ptr_array_index(t->fx_list, index) : NULL;
}
