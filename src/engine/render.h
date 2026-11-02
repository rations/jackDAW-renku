#ifndef RENDER_H_INCLUDED
#define RENDER_H_INCLUDED

#include <glib.h>
#include <sys/types.h>
#include "audio_backend.h" /* jd_nframes_t */
#include "project.h"
#include "host/pluginhost.h" /* PhMidiEvent for eng_gather_render_midi */

G_BEGIN_DECLS

/*
 * render — offline (faster-than-realtime) and realtime project bounce to an
 * audio file. The offline path drives the engine's render primitives
 * (jackdaw_engine_render_suspend + engine_track_reader_*) directly on a worker
 * thread; the realtime path taps the live post-fader master and a writer thread
 * drains it. Ported from the Linux JackDAW; adapted to this port's master model
 * (master_volume/master_muted project fields, no master FX track). Boolean
 * convention on the start functions: FALSE = success, TRUE = failure.
 */

typedef enum { RENDER_FMT_WAV, RENDER_FMT_FLAC, RENDER_FMT_MP3 } RenderFormat;
typedef enum { RENDER_BITS_16, RENDER_BITS_24, RENDER_BITS_32 } RenderBitDepth;
typedef enum { RENDER_SRC_MASTER, RENDER_SRC_SELECTED } RenderSource;
typedef enum { RENDER_SCOPE_PROJECT, RENDER_SCOPE_REGION } RenderScope;
typedef enum { RENDER_METHOD_OFFLINE, RENDER_METHOD_REALTIME } RenderMethod;

typedef struct {
    RenderFormat format;
    RenderBitDepth bit_depth; /* used only when format == WAV */
    RenderSource source;
    RenderScope scope;
    RenderMethod method;        /* OFFLINE is the default */
    int sample_rate;            /* e.g. 48000 */
    int channels;               /* 1 (mono) or 2 (stereo, default) */
    gchar *out_path;            /* full path incl. extension; owned */
    GPtrArray *selected_tracks; /* borrowed JackDawTrack* for SRC_SELECTED */
    JackDawProject *project;    /* borrowed */
} RenderOptions;

/* Shared progress/cancel block between the render worker and the UI timer. */
typedef struct {
    volatile gint cancel;       /* UI sets to request abort */
    volatile off_t frames_done; /* input (engine-SR) frames rendered so far */
    off_t frames_total;         /* total input frames in the render span */
    volatile gint finished;     /* worker sets when done/aborted */
    volatile gint failed;       /* worker sets on error (with finished) */
} JackDawRenderProgress;

void render_options_free_contents(RenderOptions *o); /* frees out_path */

/* libsndfile format from the options (0 if the combo is invalid). */
int jackdaw_render_sf_format(const RenderOptions *o);
/* TRUE if the running libsndfile can actually encode this combo. */
gboolean jackdaw_render_format_supported(const RenderOptions *o);
/* TRUE if MP3 (MPEG Layer III) encoding is available for sr/channels. */
gboolean jackdaw_render_mp3_available(int sample_rate, int channels);
/* Conventional file extension for a format ("wav"/"flac"/"mp3"). */
const char *jackdaw_render_extension(RenderFormat fmt);

/* OFFLINE: spawn a worker that renders synchronously and drives `prog`.
 * Requires the engine transport stopped. Returns the GThread (join it once
 * prog->finished), or NULL on immediate failure. Takes ownership of a deep
 * copy of `o` (caller keeps its own). */
GThread *jackdaw_render_offline_start(const RenderOptions *o, JackDawRenderProgress *prog);

/* REALTIME: open the file + writer thread, solo the selected set if needed,
 * locate + arm the master tap + start playback. Non-blocking. FALSE = success.
 * Drives `prog` via jackdaw_render_realtime_poll(). */
gboolean jackdaw_render_realtime_start(const RenderOptions *o, JackDawRenderProgress *prog);
/* Call from the UI timer while a realtime render is active: finalizes when the
 * transport reaches the end, or tears down on prog->cancel. */
void jackdaw_render_realtime_poll(JackDawRenderProgress *prog);

/* Settings persistence (last-used options). */
void jackdaw_render_options_load(RenderOptions *o); /* fills defaults + saved */
void jackdaw_render_options_save(const RenderOptions *o);

/* Engine render primitive: emit an instrument track's sequenced MIDI events for
 * [blk_start, blk_start+nframes) into `mev` (cap entries). Defined in
 * jackdaw-engine.c; declared here because its signature needs PhMidiEvent. */
int eng_gather_render_midi(JackDawTrack *t, off_t blk_start, jd_nframes_t nframes, PhMidiEvent *mev,
                           int cap);

G_END_DECLS

#endif /* RENDER_H_INCLUDED */
