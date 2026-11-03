#ifndef JACKDAW_ENGINE_H_INCLUDED
#define JACKDAW_ENGINE_H_INCLUDED

#include "audio_backend.h"
#include "project.h"
#include "track.h"

G_BEGIN_DECLS

/*
 * jackdaw-engine: one client of the audio backend (audio_backend.h; JACK),
 * named "jackdaw".
 *
 * Phase-1 registered ports:
 *   audio output: out_1 (L master), out_2 (R master)
 *   metronome:    a dedicated mono click output
 * Per-track capture, MIDI and configurable port counts return in phase 2.
 *
 * Threading contract: every non-RT entry point below is single-caller — the
 * main window's looper thread only (plus jackdaw_engine_init/quit from main()
 * before the window exists / after it quits). Other windows must route engine
 * work through the main window via BMessenger, never call in directly.
 */

/* ---- UI event hook ----
 * JACK notification threads (port registration, graph changes, server
 * shutdown) must never touch UI or emit GObject signals directly. Instead the
 * engine invokes this hook; the UI implements it as a non-blocking
 * BMessenger::SendMessage to the main window, whose handler re-reads engine
 * state. Set the hook BEFORE jackdaw_engine_init so no event is missed. */
typedef enum {
    JACKDAW_ENGINE_EVENT_PORTS_CHANGED = 1,
    JACKDAW_ENGINE_EVENT_CONNECTIONS_CHANGED,
    JACKDAW_ENGINE_EVENT_SHUTDOWN,           /* JACK server went away; engine is inactive */
    JACKDAW_ENGINE_EVENT_TAKE_FINALIZED,     /* a recorded audio take is on disk and ready
                                              * to place — call jackdaw_engine_finalize_takes()
                                              * on the main thread to drain it */
    JACKDAW_ENGINE_EVENT_MIDI_TAKE_FINALIZED /* a MIDI take was captured and the RT
                                              * thread has stopped writing — call
                                              * jackdaw_engine_finalize_midi_takes()
                                              * on the main thread to build the notes */
} JackDawEngineEvent;

typedef void (*JackDawEngineEventHook)(int event, void *user);
void jackdaw_engine_set_event_hook(JackDawEngineEventHook hook, void *user);

/* Initialise the engine and activate the JACK client.
 * Must be called from the main thread before adding any tracks.
 * Returns FALSE on success, TRUE on failure. */
gboolean jackdaw_engine_init(JackDawProject *project);

/* Deactivate and close the JACK client. Safe to call even if not initialised. */
void jackdaw_engine_quit(void);

/* Returns TRUE if the JACK client is currently active. */
gboolean jackdaw_engine_is_running(void);

/* Returns TRUE while ENGINE_RECORDING flag is set. */
gboolean jackdaw_engine_is_recording(void);

/* Returns TRUE while ENGINE_PLAYING flag is set. */
gboolean jackdaw_engine_is_playing(void);

/* --- Track management --- */
/* Add a track to the engine; allocates track->slot. Returns FALSE on success,
 * TRUE on failure. */
gboolean jackdaw_engine_add_track(JackDawTrack *track);
void jackdaw_engine_remove_track(JackDawTrack *track);

/* --- Input routing (main thread only) ---
 * Connect an external JACK output port to a track's capture port and remember
 * the connection so it can be torn down again. port_name == NULL clears the
 * source. Return FALSE on success, TRUE on failure. set_track_stereo registers
 * or drops the track's right capture port (must precede wiring the right
 * source). */
gboolean jackdaw_engine_set_audio_source_l(JackDawTrack *t, const gchar *port_name);
gboolean jackdaw_engine_set_audio_source_r(JackDawTrack *t, const gchar *port_name);
gboolean jackdaw_engine_set_track_stereo(JackDawTrack *t, gboolean stereo);
/* Connect an external MIDI output to an instrument track's MIDI capture port
 * (routing only in this phase; thru/record land with the MIDI phase). */
gboolean jackdaw_engine_set_midi_source(JackDawTrack *t, const gchar *port_name);

/* NULL-terminated lists of connectable sources (physical hardware ports). Audio
 * capture and MIDI capture are JackPortIsOutput|Physical of their type. Free
 * with jackdaw_engine_free_ports. NULL when the engine is inactive. */
const char **jackdaw_engine_list_capture_ports(void);
const char **jackdaw_engine_list_midi_ports(void);
void jackdaw_engine_free_ports(const char **ports);

/* --- MIDI control surface input (main thread only) ---
 * The engine registers a dedicated MIDI input port "control_in", separate from
 * per-track recording/instrument routing. A footswitch / CC device connects
 * here; the process callback copies its events into a lock-free ring that the
 * main thread drains (see midicontrol.c). */
typedef struct {
    guint8 size;
    guint8 data[3];
} JackDawCtlEvent;
/* Dequeue one buffered control event. Returns FALSE when none are pending. */
gboolean jackdaw_engine_control_poll(JackDawCtlEvent *out);
/* Connect (port_name) or disconnect (NULL/empty) an external MIDI source to
 * control_in. port_name comes only from jackdaw_engine_list_midi_ports().
 * Returns FALSE on success, TRUE on failure. */
gboolean jackdaw_engine_set_control_source(const gchar *port_name);
/* The currently connected control source port name, or NULL. Borrowed. */
const gchar *jackdaw_engine_get_control_source(void);

/* --- Audio port-count management (main thread only) ---
 * Grow/shrink the pool of JACK capture (in_N) and master output (out_N) ports,
 * clamped to [1, JACKDAW_MAX_TRACKS] and persisted for the next session. The
 * count change is RT-safe (no reallocation; ordered publish) — see the .c. A
 * track added afterwards at a slot within the input pool gains a capture port.
 * MIDI ports are not managed here: instrument tracks register their own MIDI
 * in/out ports on demand, so there is no fixed MIDI pool to resize. Return
 * FALSE on success, TRUE on failure. */
guint jackdaw_engine_get_audio_in_count(void);
guint jackdaw_engine_get_audio_out_count(void);
gboolean jackdaw_engine_set_audio_in_count(guint n);
gboolean jackdaw_engine_set_audio_out_count(guint n);

/* --- Transport --- */
void jackdaw_engine_start_playback(void);
void jackdaw_engine_stop_playback(void);
void jackdaw_engine_start_recording(void);
void jackdaw_engine_stop_recording(void);
/* Sound `beats` metronome clicks, then start playback (record=FALSE) or
 * recording (record=TRUE). Returns FALSE if no pre-roll was started (caller
 * starts now). */
gboolean jackdaw_engine_begin_countin(guint beats, gboolean record);
void jackdaw_engine_locate(off_t sample);

/* --- Loop region ---
 * A [start, end) frame span the playhead wraps back over while looping is
 * enabled. The wrap fires only once the playhead has entered the region
 * (block-start in [start, end)); a playhead placed after the region plays
 * straight through. set_loop_range normalises start <= end. All accesses are
 * atomic (written on the window thread, read by the RT callback). */
void jackdaw_engine_set_loop_range(off_t start, off_t end);
void jackdaw_engine_get_loop_range(off_t *start, off_t *end);
void jackdaw_engine_set_loop_enabled(gboolean on);
gboolean jackdaw_engine_get_loop_enabled(void);
gboolean jackdaw_engine_has_loop_region(void); /* TRUE when loop_end > loop_start */

/* --- Record mode ---
 * RECORD_MODE_PUNCH auto-records armed audio tracks over the loop-tab region
 * [loop_start, loop_end) (wired in the recording phase); RECORD_MODE_NORMAL
 * records from the cursor. Stored here so the RT capture path can read it. */
enum { RECORD_MODE_NORMAL = 0, RECORD_MODE_PUNCH = 1 };
void jackdaw_engine_set_record_mode(int mode);
int jackdaw_engine_get_record_mode(void);

/* Drain any finished recorded takes into placed regions. Call on the MAIN
 * thread in response to JACKDAW_ENGINE_EVENT_TAKE_FINALIZED: it creates the
 * AudioClips and edits the track region lists (which are main-thread-owned).
 * No-op when nothing is pending. */
void jackdaw_engine_finalize_takes(void);

/* Build recorded MIDI into clip notes. Call on the MAIN thread in response to
 * JACKDAW_ENGINE_EVENT_MIDI_TAKE_FINALIZED: it drains each armed instrument
 * track's capture ringbuffer into MidiNotes, merges them into the track clip and
 * republishes the RT snapshot. No-op when nothing was captured. */
void jackdaw_engine_finalize_midi_takes(void);

/* --- Live MIDI recording preview (main thread / draw only) ---
 * One in-progress recorded note, in ABSOLUTE timeline frames. Note-ons are
 * paired with note-offs; notes still held are extended to the current playhead. */
typedef struct {
    off_t start_frame, end_frame;
    guint8 pitch, velocity, channel;
} JackDawRecNote;
/* Non-destructively peek the MIDI captured so far for `t` (instrument track being
 * recorded). Returns a borrowed pointer to a static array valid until the next
 * call, with *count set; NULL/0 if nothing is being recorded. */
const JackDawRecNote *jackdaw_engine_rec_preview(JackDawTrack *t, guint *count);

/* --- Preview note (main thread only) ---
 * Inject a single live note-on (on=TRUE) or note-off (on=FALSE) onto an
 * instrument track's MIDI output, played immediately regardless of transport
 * state. Lock-free: queued to the RT thread via a ringbuffer. Used by the
 * piano-roll keyboard to audition pitches. `channel` (0-15) selects the MIDI
 * channel so previews reach the same voice as the clip's notes (e.g. drums on
 * channel 10). No-op if the track is not an instrument track registered with
 * the engine. */
void jackdaw_engine_preview_note(JackDawTrack *t, guint8 pitch, guint8 velocity, guint8 channel,
                                 gboolean on);

/* TRUE while a count-in pre-roll is sounding (transport not yet engaged). */
gboolean jackdaw_engine_is_counting_in(void);

/* Sample rate reported by JACK (valid after jackdaw_engine_init) */
jd_nframes_t jackdaw_engine_get_sample_rate(void);
jd_nframes_t jackdaw_engine_get_buffer_size(void);
/* The longest period the engine is ready for without reallocating: the
 * current one or JD_MAX_PERIOD, whichever is longer. Plug-ins are set up for
 * blocks of this length, so a later period change still runs them. */
jd_nframes_t jackdaw_engine_get_max_buffer_size(void);

/* Monotonically-increasing playback position in samples.
 * Reset by jackdaw_engine_locate(); increments nframes per process cycle
 * while ENGINE_PLAYING is set. Read by main thread for display only. */
off_t jackdaw_engine_get_play_pos(void);

/* Post-master-fader peak levels (master VU). Resets the stored peak on read. */
void jackdaw_engine_get_master_peaks(gfloat *out_L, gfloat *out_R);

/* Cumulative xrun count reported by JACK since engine init (display only). */
guint jackdaw_engine_get_xrun_count(void);

/* Process callbacks completed since engine init (wraps; compare differences,
 * never absolutes). Lets non-RT code establish that the RT thread has run again
 * since a structure was retired — see jackdaw_track_fx_collect. */
guint jackdaw_engine_get_cycle_count(void);

/* TRUE while the JACK client is active, i.e. while the process callback can
 * still be running. FALSE means nothing is reading the RT graph. */
gboolean jackdaw_engine_is_processing(void);

/* ---- Render / export support (P11) ----
 * The offline render (render.c) reads track audio synchronously through an
 * EngTrackReader and mixes on a worker thread while the RT graph is held off
 * the plugins via render_suspend. The realtime bounce taps the post-fader
 * master into a ring drained by a writer thread. jackdaw_engine_set_suspended
 * uses the same suspend flag to freeze the graph while a project load rebuilds
 * the FX chains. eng_gather_render_midi is declared in render.h (its signature
 * needs PhMidiEvent). */
typedef struct EngTrackReader EngTrackReader;
EngTrackReader *engine_track_reader_new(JackDawTrack *t, int render_sr);
void engine_track_reader_free(EngTrackReader *r);
gboolean engine_track_reader_read(EngTrackReader *r, JackDawTrack *t, off_t start, jd_nframes_t n,
                                  float *outL, float *outR);

void jackdaw_engine_render_suspend(gboolean on);
void jackdaw_engine_set_suspended(gboolean on);

void jackdaw_engine_render_tap_start(off_t end_frame);
void jackdaw_engine_render_tap_stop(void);
gboolean jackdaw_engine_render_tap_done(void);

/* Absolute path to the scratch-recordings directory
 * (<settings>/JackDAW/recordings), created if missing. Caller g_free()s it. */
gchar *jackdaw_recordings_dir(void);
size_t jackdaw_engine_render_tap_read(float *L, float *R, size_t max_frames);

G_END_DECLS

#endif /* JACKDAW_ENGINE_H_INCLUDED */
