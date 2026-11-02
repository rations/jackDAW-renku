#ifndef TRACK_H_INCLUDED
#define TRACK_H_INCLUDED

#include <glib-object.h>
#include "rt_ringbuffer.h"

#include "audio_clip.h"
#include "clipregion.h"
#include "midiclip.h"

G_BEGIN_DECLS

/* Track kind: an audio track streams AudioClip regions; an instrument track
 * sequences MidiRegions into its first FX-chain plugin (the instrument).
 * Audio clip regions, MIDI and the FX chain are present; routing MIDI into an
 * instrument plugin is a later phase. */
typedef enum { JACKDAW_TRACK_AUDIO = 0, JACKDAW_TRACK_INSTRUMENT } JackDawTrackKind;

#define JACKDAW_TYPE_TRACK (jackdaw_track_get_type())
#define JACKDAW_TRACK(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, JACKDAW_TYPE_TRACK, JackDawTrack))
#define JACKDAW_IS_TRACK(obj) (G_TYPE_CHECK_INSTANCE_TYPE(obj, JACKDAW_TYPE_TRACK))

/* State flag bits — written by main thread, read by RT callback */
#define TRACK_ARMED (1 << 0)
#define TRACK_MUTED (1 << 1)
#define TRACK_SOLOED (1 << 2)

/* Max tracks the engine supports */
#define JACKDAW_MAX_TRACKS 64

/* MIDI capture ringbuffer size — event-based, not sample-rate-dependent. */
#define TRACK_MIDI_RINGBUF_BYTES 65536

typedef struct _JackDawTrack JackDawTrack;
typedef struct _JackDawTrackClass JackDawTrackClass;

struct _JackDawTrack {
    GObject parent_instance;

    gchar *name;
    guint slot;            /* index in engine track slot array */
    JackDawTrackKind kind; /* audio (default) or instrument */

    /* Input routing — indices into engine port arrays; -1 = none. */
    gint audio_in_idx;
    gint midi_in_idx;

    /* External JACK ports connected to this track's capture ports (main-thread
     * only; NULL = no jackdaw-made connection). audio_src_port is the left/mono
     * source; audio_src_port_r is the right source (NULL when mono). */
    gchar *audio_src_port;
    gchar *audio_src_port_r;
    gchar *midi_src_port;  /* external MIDI source (instrument tracks); NULL = none */
    gboolean stereo_input; /* TRUE = the track captures a stereo pair */

    /* MIDI clip model (instrument tracks; main-thread). Notes are stored in the
     * source clip at musical ticks; midi_regions window into it and drive the
     * immutable rt_midi event snapshot the RT callback reads (published lock-free
     * like rt_snapshot). rt_midi is a MidiEventSnapshot* swapped atomically; the
     * previous one is deferred on retire_midi and freed on the next commit. */
    MidiClip *midi_clip;     /* single source clip (all notes) */
    GPtrArray *midi_regions; /* GPtrArray of MidiRegion* windowing into midi_clip */
    gpointer rt_midi;        /* MidiEventSnapshot* (atomic) */
    GPtrArray *retire_midi;  /* MidiEventSnapshot* awaiting free */

    /* MIDI capture ringbuffer: written by the RT callback while recording an
     * armed instrument track, drained on the main thread when recording stops
     * (turned into clip notes). Allocated with the play buffers. */
    rt_ringbuffer_t *midi_rec_buf;

    /* RT-safe state: written atomically by main thread */
    volatile gint32 state_flags;
    volatile gfloat volume; /* EFFECTIVE gain (= trim * fader), RT reads this */
    volatile gfloat trim;   /* input trim gain (track strip dial) */
    volatile gfloat fader;  /* channel fader gain (mixer fader) */
    volatile gfloat pan;    /* -1.0 (L) … 0.0 (C) … 1.0 (R) */

    /* Peak metering: written by RT callback, read by main thread */
    volatile gfloat peak_L;
    volatile gfloat peak_R;

    /* Timeline clip regions (main-thread list, ordered by tl_pos).
     * The feeder thread never touches this directly — it reads the immutable
     * rt_snapshot under region_lock. */
    GPtrArray *regions;              /* GPtrArray of ClipRegion* */
    GMutex region_lock;              /* guards rt_snapshot (main ↔ feeder) */
    ClipRegionSnapshot *rt_snapshot; /* current snapshot for the feeder */

    /* Timeline position used as the default placement for set_clip and as the
     * live-recording anchor (recording finalisation is a later phase). */
    off_t clip_start;

    /* Playback ringbuffers: fed by the feeder thread, drained by RT callback.
     * Allocated by jackdaw_engine_add_track() once the JACK rate is known. */
    rt_ringbuffer_t *play_buf_L;
    rt_ringbuffer_t *play_buf_R;

    /* Capture ringbuffers: written by the RT callback while recording, drained
     * by the recorder thread into a WAV file. Allocated with the play buffers.
     * A mono track duplicates its one input into both. */
    rt_ringbuffer_t *rec_buf_L;
    rt_ringbuffer_t *rec_buf_R;

    /* Recording anchor (timeline frame the take begins at) and the input port's
     * capture latency in frames, used to align the finalised clip under the
     * audio the performer actually played against. Set on the main thread when
     * a capture slot is opened. */
    off_t rec_start_frame;
    off_t rec_latency;

    /* Live record waveform: one (min,max) float pair per JACK period, written
     * by the RT callback while recording and read by the UI overlay. Allocated
     * on record-arm, freed when the take is finalised. rec_peak_count only ever
     * grows during a take (racy UI reads are safe); rec_peak_block is the
     * frames-per-bucket (the JACK buffer size). */
    volatile gfloat *rec_peak_buf;
    volatile gint rec_peak_count;
    gint rec_peak_block;

    /* Feeder's playback position in timeline frames (feeder-owned; the RT
     * callback does not read it). */
    volatile off_t played_frames;

    /* FX: main-thread list of PluginInstance* (effects on this track). */
    GPtrArray *fx_list;

    /* Immutable FX chain snapshot read by the RT callback (JackDawFxChain*),
     * published with g_atomic_pointer_set. Old chains/instances are reclaimed
     * on the NEXT edit (deferred so the RT thread is never left dangling). */
    gpointer rt_chain;
    GPtrArray *retire_chains; /* JackDawFxChain* awaiting free */
    GPtrArray *retire_fx;     /* PluginInstance* awaiting free */
    guint retire_cycle;       /* engine cycle count when retire_fx last grew */
};

/* Immutable FX chain snapshot. fx[i] is a PluginInstance* (opaque here, so
 * track.h consumers don't pull in the plugin host headers). */
typedef struct {
    int n;
    gpointer *fx;
} JackDawFxChain;

struct _JackDawTrackClass {
    GObjectClass parent_class;

    void (*state_changed)(JackDawTrack *track);
    void (*routing_changed)(JackDawTrack *track);
};

GType jackdaw_track_get_type(void);

/* Constructor — the track takes ownership of clip (may be NULL); a non-NULL
 * clip is placed as a single region at timeline position 0. */
JackDawTrack *jackdaw_track_new(const gchar *name, AudioClip *clip);

/* Accessors (main-thread safe) */
const gchar *jackdaw_track_get_name(JackDawTrack *t);
void jackdaw_track_set_name(JackDawTrack *t, const gchar *name);

/* ---- Clip regions (main thread only) ----
 * Returns the first region's source clip, or NULL — borrowed, do not free.
 * Back-compat helper; new code iterates jackdaw_track_get_regions(). */
AudioClip *jackdaw_track_get_clip(JackDawTrack *t);
/* Replace all regions with a single region holding new_clip placed at clip_start.
 * Consumes one reference to new_clip (take-ownership semantics). */
void jackdaw_track_set_clip(JackDawTrack *t, AudioClip *new_clip);
/* Place a clip as a new region at timeline position tl_pos, spanning the whole
 * file. Consumes one reference to clip. Rebuilds the feeder snapshot. */
void jackdaw_track_place_clip(JackDawTrack *t, AudioClip *clip, off_t tl_pos);

/* Borrowed region list — edit in place, then call jackdaw_track_commit_regions(). */
GPtrArray *jackdaw_track_get_regions(JackDawTrack *t);

/* Rebuild + publish the immutable feeder snapshot from the current region list
 * and emit state-changed so wave views redraw. Call after any region edit. */
void jackdaw_track_commit_regions(JackDawTrack *t);

/* Replace the region list wholesale with a copy of `list` (may be NULL/empty)
 * and republish the feeder snapshot. Used by undo restore. */
void jackdaw_track_apply_regions(JackDawTrack *t, GPtrArray *list);

/* Last timeline frame covered by any region (0 if empty). */
off_t jackdaw_track_total_frames(JackDawTrack *t);

/* Feeder-thread access: take/drop a reference to the current snapshot.
 * ref locks region_lock briefly; the returned snapshot is stable until unref. */
ClipRegionSnapshot *jackdaw_track_ref_snapshot(JackDawTrack *t);

/* ---- Track kind (main thread) ---- */
JackDawTrackKind jackdaw_track_get_kind(JackDawTrack *t);
void jackdaw_track_set_kind(JackDawTrack *t, JackDawTrackKind kind);
gboolean jackdaw_track_is_instrument(JackDawTrack *t);

/* ---- MIDI clip / regions (main thread; instrument tracks) ----
 * get_midi_clip / get_midi_regions return borrowed pointers — edit the region
 * list or notes in place, then call jackdaw_track_commit_midi() to rebuild and
 * publish the immutable RT event snapshot. ensure_midi_region seeds the default
 * full-clip region if the list is empty (so the whole clip is grabbable). */
MidiClip *jackdaw_track_get_midi_clip(JackDawTrack *t);
GPtrArray *jackdaw_track_get_midi_regions(JackDawTrack *t);
void jackdaw_track_ensure_midi_region(JackDawTrack *t);

/* Rebuild + publish the RT MIDI event snapshot from the region list. Converts
 * ticks to frames with frames_per_beat (= sample_rate * 60 / bpm). */
void jackdaw_track_commit_midi(JackDawTrack *t, double frames_per_beat);

/* Replace the track's MIDI clip wholesale (consumes `clip`; frees the old one)
 * and republish the snapshot. Used by editor undo/redo restore. */
void jackdaw_track_set_midi_clip(JackDawTrack *t, MidiClip *clip, double frames_per_beat);

/* Replace the MIDI region list with deep copies from `list` (borrowed) and
 * republish the snapshot. MIDI counterpart of jackdaw_track_apply_regions(),
 * used by timeline region-edit undo/redo restore. */
void jackdaw_track_apply_midi_regions(JackDawTrack *t, GPtrArray *list, double frames_per_beat);

/* State flag helpers — use g_atomic_int_or/and for thread safety */
void jackdaw_track_set_armed(JackDawTrack *t, gboolean armed);
void jackdaw_track_set_muted(JackDawTrack *t, gboolean muted);
void jackdaw_track_set_soloed(JackDawTrack *t, gboolean soloed);
gboolean jackdaw_track_is_armed(JackDawTrack *t);
gboolean jackdaw_track_is_muted(JackDawTrack *t);
gboolean jackdaw_track_is_soloed(JackDawTrack *t);

/* Volume/pan — stored as volatile float, main thread writes, RT reads */
/* Effective gain = trim * fader. set_volume() is legacy (sets the fader stage
 * with trim left at unity); get_volume() returns the effective product. */
void jackdaw_track_set_volume(JackDawTrack *t, gfloat vol);
gfloat jackdaw_track_get_volume(JackDawTrack *t);

/* Two independent gain stages (gain staging): the track-strip dial drives the
 * trim; the mixer fader drives the fader. Both fold into the effective volume. */
void jackdaw_track_set_trim(JackDawTrack *t, gfloat trim);
gfloat jackdaw_track_get_trim(JackDawTrack *t);
void jackdaw_track_set_fader(JackDawTrack *t, gfloat fader);
gfloat jackdaw_track_get_fader(JackDawTrack *t);
void jackdaw_track_set_pan(JackDawTrack *t, gfloat pan);
gfloat jackdaw_track_get_pan(JackDawTrack *t);

/* Input routing */
void jackdaw_track_set_audio_in(JackDawTrack *t, gint idx);
void jackdaw_track_set_midi_in(JackDawTrack *t, gint idx);

/* Stereo-input flag (main thread). Mono is the default. */
gboolean jackdaw_track_is_stereo(JackDawTrack *t);

/* Peak meter read (call from main thread) */
void jackdaw_track_get_peaks(JackDawTrack *t, gfloat *out_L, gfloat *out_R);

/* ---- FX chain (main thread) ----
 * Instances are PluginInstance* (from pluginhost.h); kept as gpointer here to
 * avoid pulling plugin-host headers into every track.h consumer. fx_add takes
 * ownership; fx_remove defers the pluginhost_free until the RT thread has
 * moved past the retired chain snapshot. Every edit atomically republishes
 * the immutable rt_chain the RT callback reads. */
void jackdaw_track_fx_add(JackDawTrack *t, gpointer instance);
void jackdaw_track_fx_remove(JackDawTrack *t, guint index);
void jackdaw_track_fx_move(JackDawTrack *t, guint from, guint to);
guint jackdaw_track_fx_count(JackDawTrack *t);
gpointer jackdaw_track_fx_get(JackDawTrack *t, guint index);

/* Free instances retired by an earlier fx edit, once the RT thread has run
 * again since they were retired (so nothing can still reach them through the
 * chain snapshot that referenced them). Returns TRUE when the retire queue is
 * empty afterwards; FALSE means "not yet, call again".
 *
 * Without this, a removed plug-in is only freed by the NEXT edit on the same
 * track — which for a Wine-bridged plug-in leaves its Wine host process running
 * until the track is destroyed. Must be called on the thread that instantiated
 * the plug-ins (their FX window's looper), like pluginhost_free itself. */
gboolean jackdaw_track_fx_collect(JackDawTrack *t);

G_END_DECLS

#endif /* TRACK_H_INCLUDED */
