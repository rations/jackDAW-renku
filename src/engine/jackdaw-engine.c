#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

#include <glib/gstdio.h>

#include <jack/jack.h>
#include "rt_ringbuffer.h"
#include <jack/midiport.h>
#include <jack/thread.h> /* jack_client_create_thread — RT-priority worker pool */
#include <sndfile.h>
#include <samplerate.h>

#include <FindDirectory.h> /* find_directory — recordings dir, no dot-folders */
#include <StorageDefs.h>   /* B_PATH_NAME_LENGTH */

#include "rt_denormal.h" /* FTZ/DAZ for the RT thread */

#include "jackdaw-engine.h"
#include "host/pluginhost.h"
#include "settings.h"
#include "message.h"

/* -----------------------------------------------------------------------
 * Internal state
 *
 * Ported from the Linux JackDAW engine: transport flags + play_pos, count-in,
 * metronome, stereo master outs, per-track capture ports, loop wrap, and the
 * playback feeder thread that streams clip regions into the RT drain. The
 * recorder thread, MIDI ports, plugin graph and render taps of the original
 * return in later phases; their state fields keep their slots so that code
 * drops back in unchanged.
 * ----------------------------------------------------------------------- */

/* Transport control flags — written by main thread, read by RT callback.
 * Use g_atomic_int_* for all accesses. */
#define ENGINE_PLAYING (1 << 0)
#define ENGINE_RECORDING (1 << 1)

typedef struct {
    jack_client_t *client;
    JackDawProject *project; /* weak ref — project owns the engine */

    /* Audio output ports — out_1 (L master), out_2 (R master). */
    jack_port_t **audio_out;
    guint audio_out_count;

    /* Audio capture ports — a pool of mono inputs. audio_in[i] is track i's
     * LEFT/mono capture; audio_in_r[i] its RIGHT capture (stereo tracks only).
     * A track reads the pair at its audio_in_idx. */
    jack_port_t **audio_in;
    jack_port_t **audio_in_r;
    guint audio_in_count;

    /* MIDI capture ports, one per instrument-track slot, registered lazily when
     * an instrument track is added (midi_in[slot]). A NULL slot has no MIDI
     * input. Sized JACKDAW_MAX_TRACKS. */
    jack_port_t **midi_in;

    /* MIDI playback ports, one per instrument-track slot (midi_out[slot]),
     * registered with the matching midi_in when an instrument track is added.
     * Each instrument track's scheduled notes, live thru (while armed) and
     * preview notes are merged and written here. Sized JACKDAW_MAX_TRACKS. */
    jack_port_t **midi_out;

    /* Dedicated control-surface MIDI input ("control_in"), separate from the
     * per-track MIDI inputs. A footswitch / CC device connects here; the RT
     * callback copies its events into eng_control_rb for the main thread to
     * interpret (see midicontrol.c). control_src_port is the currently connected
     * external source name (owned; main thread only). */
    jack_port_t *control_in;
    gchar *control_src_port;

    /* Pre-allocated mix buffers (sized to buffer size at init) */
    float *master_L;
    float *master_R;
    /* Per-track scratch, reused sequentially (single-threaded mix pass). */
    /* Per-slot scratch for PARALLEL track processing: each track's worker writes
     * its post-fader stereo contribution into slot_L[i]/slot_R[i]; the RT thread
     * sums them into master after the barrier. Sized to buf_size (init +
     * buffer_size_cb). Each slot is touched by at most one worker per cycle. */
    float *slot_L[JACKDAW_MAX_TRACKS];
    float *slot_R[JACKDAW_MAX_TRACKS];
    /* The gain each slot ended its last cycle on, so a mute or fader change
     * ramps across one period instead of stepping. Same ownership as slot_L/R:
     * only the worker processing slot i this cycle reads or writes it. A track
     * that takes over a freed slot starts from the old track's last gain; that
     * costs one period's ramp, never a jump. */
    float slot_gain_L[JACKDAW_MAX_TRACKS];
    float slot_gain_R[JACKDAW_MAX_TRACKS];
    jack_nframes_t buf_size; /* current buffer size */

    /* Weak refs to active tracks — slots populated by engine_add_track */
    JackDawTrack *slots[JACKDAW_MAX_TRACKS];

    volatile gint transport_flags; /* ENGINE_PLAYING | ENGINE_RECORDING */
    volatile off_t play_pos;       /* sample counter, incremented by process cb */

    /* Loop region (frames). Looping is active only while loop_enabled is set and
     * loop_end > loop_start; the playhead wraps loop_end -> loop_start once it
     * has entered the region. Written on window threads, read by RT. start/end
     * are a pair: other threads read them through eng_loop_read(). */
    volatile gint loop_enabled;
    volatile off_t loop_start;
    volatile off_t loop_end;
    volatile gint loop_seq; /* odd while set_loop_range is mid-update */

    /* Record mode (RECORD_MODE_NORMAL / RECORD_MODE_PUNCH). Punch capture over
     * the loop-tab region is wired in the recording phase; stored here now so
     * the RT capture path can read it. Window thread writes, RT reads. */
    volatile gint record_mode;

    /* Punch in/out. record_mode selects it; punch_armed is set on the main
     * thread when playback starts in punch mode with a tab region ahead of the
     * playhead, and cleared by the RT path (or a stop) once the punch completes.
     * While armed the RT path auto-engages RECORDING across [loop_start,loop_end). */
    volatile gint punch_armed;

    /* Count-in pre-roll. While countin_active the transport is in a metronome-
     * only lead-in: the project is frozen (play_pos does not advance) and nothing
     * records. The metronome clicks from countin_pos; when it reaches countin_len
     * the pending transport (PLAYING, plus RECORDING if countin_pending_rec)
     * engages and normal play begins from the unchanged play_pos. Set up on the
     * main thread; the RT path drives countin_pos and performs the hand-off. */
    volatile gint countin_active;
    volatile gint countin_pending_rec;
    volatile off_t countin_pos;
    volatile off_t countin_len;

    jack_nframes_t sample_rate; /* cached at init */

    /* Pre-rendered metronome click (mono), built at init. */
    float *click_buf;
    int click_len;

    /* Dedicated mono metronome output port ("metronome"). The click is always
     * mirrored here so it can be routed to a performer's headphones independently
     * of the main mix; whether it ALSO reaches the main outs is set per-project
     * (metronome_route). Never part of the master sum or meters. */
    jack_port_t *metro_out;

    /* Post-master-fader peak meter (master VU). Racy read is acceptable. */
    volatile gfloat master_peak_L;
    volatile gfloat master_peak_R;

    /* Render support (P11 export + project load). While render_suspend is set
     * the RT callback outputs silence and touches no plugin, so the main thread
     * can drive the offline render (which owns every PluginInstance) or tear
     * down / rebuild the FX graph on load without racing the audio thread.
     * render_active taps the post-master-fader mix into render_rb_L/R for the
     * realtime bounce; render_done is set once play_pos reaches render_end. */
    volatile gint render_suspend;
    volatile gint render_active;
    volatile gint render_done;
    off_t render_end;
    float *render_tap_L;
    float *render_tap_R;
    rt_ringbuffer_t *render_rb_L;
    rt_ringbuffer_t *render_rb_R;

    gboolean active;
} JackDawEngine;

static JackDawEngine engine;

static volatile gint engine_xruns = 0;
/* Process-callback counter, incremented once per cycle on the RT thread. Reused
 * by non-RT code that must know the RT thread has moved past a retired
 * structure (see jackdaw_track_fx_collect). A lock-free atomic increment is the
 * only RT cost. */
static volatile gint engine_cycles = 0;

/* Diagnostics (JACKDAW_DIAG): per-cycle callback timing + xrun reporter, so a
 * missed deadline can be attributed to this callback vs. external scheduling. */
static gboolean g_diag_on = FALSE;
static volatile gint64 g_diag_cb_last_us = 0;
static volatile gint64 g_diag_cb_max_us = 0;
static gint64 g_diag_period_us = 0;
static GThread *g_diag_thread = NULL;
static volatile gint g_diag_quit = 0;
/* MIDI-path diagnostics (JACKDAW_DIAG): raw events seen on instrument midi_in
 * ports, events written to the record ring, and the last instrument track's RT
 * state = (armed<<0)|(recording<<1)|(playing<<2). Isolates a dead JACK read from
 * a wrong gate. */
static volatile gint g_diag_midi_in = 0;
static volatile gint g_diag_midi_rec = 0;
static volatile gint g_diag_instr_state = -1;
/* Per-plugin worst-case µs inside process(), written by whichever RT thread
 * owns the track this cycle and read-and-reset by the diag thread. Indexed
 * [track slot][chain position] so no off-RT code ever has to walk (and race
 * with the retire of) a track's rt_chain snapshot. A plugin past the slot
 * count is timed into the last slot rather than dropped.
 *
 * gint (not gint64) so the accesses can use glib's portable atomics: a block
 * period is microseconds, far inside 32 bits. The "keep the larger" update is
 * deliberately not a compare-and-swap — two RT threads racing on one element
 * can at worst drop a sample from a diagnostic maximum, which is not worth a
 * CAS loop on the audio path. */
#define ENG_DIAG_FX_SLOTS 8
static volatile gint g_diag_fx_us[JACKDAW_MAX_TRACKS][ENG_DIAG_FX_SLOTS];

static gpointer diag_thread_func(gpointer arg)
{
    (void)arg;
    int last_xruns = 0;
    while (!g_atomic_int_get(&g_diag_quit)) {
        g_usleep(1000000); /* 1 s */
        int x = g_atomic_int_get(&engine_xruns);
        int dx = x - last_xruns;
        last_xruns = x;
        gint64 cb_max = g_diag_cb_max_us;
        g_diag_cb_max_us = 0;
        g_message("[diag] xruns +%d (total %d)  cb_last=%ldus cb_max=%ldus period=%ldus", dx, x,
                  (long)g_diag_cb_last_us, (long)cb_max, (long)g_diag_period_us);
        /* MIDI path: raw events seen on instrument midi_in ports, events written
         * to the record ring, and the gate state bitmask
         * (1=armed 2=recording 4=playing 8=rec-ring-allocated; -1 = no
         * instrument track with a registered midi_in reached the RT pass). */
        g_message("[diag] midi_in=%d midi_rec=%d instr_state=%d", g_atomic_int_get(&g_diag_midi_in),
                  g_atomic_int_get(&g_diag_midi_rec), g_atomic_int_get(&g_diag_instr_state));
        /* Per-plugin worst case for the last second, as µs and as a share of
         * the period budget — this is what attributes an xrun to one plugin
         * rather than to the callback as a whole. */
        for (guint ti = 0; ti < JACKDAW_MAX_TRACKS; ti++) {
            for (int fi = 0; fi < ENG_DIAG_FX_SLOTS; fi++) {
                gint us = g_atomic_int_get(&g_diag_fx_us[ti][fi]);
                if (us <= 0)
                    continue;
                g_atomic_int_set(&g_diag_fx_us[ti][fi], 0);
                g_message("[diag]   track %u fx[%d] max=%ldus (%.1f%% of period)", ti, fi, (long)us,
                          g_diag_period_us > 0 ? 100.0 * (double)us / (double)g_diag_period_us
                                               : 0.0);
            }
        }
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Playback feeder thread
 *
 * A dedicated pthread fills each track's play_buf_L/R ringbuffers from its
 * AudioClip regions so the RT callback always has audio to drain. This is NOT
 * an RT thread: it may allocate, open files (libsndfile) and resample
 * (libsamplerate); the only contract it shares with the RT callback is the
 * lock-free rt_ringbuffer. Ported from the Linux JackDAW engine; the
 * HAVE_SAMPLERATE guards are dropped since libsamplerate is always present on
 * the Haiku target.
 * ----------------------------------------------------------------------- */

/* Output frames produced per slot per inner-loop pass (controls granularity). */
#define FEEDER_CHUNK_FRAMES 2048
/* Raw interleaved frames for one read (covers up to 6:1 downsample). */
#define FEEDER_RAW_FRAMES (FEEDER_CHUNK_FRAMES * 6)
/* Maximum clip channels we deinterleave (ch 0 and 1 are used). */
#define FEEDER_MAX_CHANNELS 8

/* Per-slot feeder state — only the atomic locate fields are shared with the
 * main thread; everything else is private to the feeder thread. */
typedef struct {
    SNDFILE *sf;      /* open file for the current region; NULL = none */
    int open_clip_sr; /* sample rate of the open file */
    int open_clip_ch; /* channel count of the open file */
    SRC_STATE *src_L; /* resampler for channel 0; NULL = no SRC */
    SRC_STATE *src_R; /* resampler for channel 1; NULL = mono or no SRC */
    /* Locate protocol: main thread writes locate_frame then sets locate_req=1.
     * Feeder does CAS(1->0) and applies the seek. */
    volatile gint locate_req;
    off_t locate_frame;       /* timeline frame target; valid when locate_req==1 */
    off_t play_frame;         /* feeder's current timeline frame position */
    ClipRegionSnapshot *snap; /* snapshot the feeder currently holds a ref to */
    int open_region;          /* index into snap of the open region; -1 = none */
} FeederSlot;

static FeederSlot feeder_slots[JACKDAW_MAX_TRACKS];
static pthread_t feeder_tid;
static volatile int feeder_stop_flag;
static gboolean feeder_started = FALSE;

/* Scratch buffers owned exclusively by the feeder thread */
static float *feeder_raw;  /* FEEDER_RAW_FRAMES * FEEDER_MAX_CHANNELS interleaved */
static float *feeder_mono; /* FEEDER_RAW_FRAMES — one deinterleaved channel pre-SRC */
static float *feeder_L;    /* FEEDER_CHUNK_FRAMES — output left channel */
static float *feeder_R;    /* FEEDER_CHUNK_FRAMES — output right channel */

/* Close a slot's open file and SRC states (feeder thread or stop path only).
 * Leaves play_frame and the held snapshot untouched. */
static void feeder_slot_close(guint i)
{
    if (feeder_slots[i].sf) {
        sf_close(feeder_slots[i].sf);
        feeder_slots[i].sf = NULL;
    }
    if (feeder_slots[i].src_L) {
        src_delete(feeder_slots[i].src_L);
        feeder_slots[i].src_L = NULL;
    }
    if (feeder_slots[i].src_R) {
        src_delete(feeder_slots[i].src_R);
        feeder_slots[i].src_R = NULL;
    }
    feeder_slots[i].open_region = -1;
}

/* Full release: file handle, SRC, and snapshot reference. */
static void feeder_slot_release(guint i)
{
    feeder_slot_close(i);
    if (feeder_slots[i].snap) {
        clip_region_snapshot_unref(feeder_slots[i].snap);
        feeder_slots[i].snap = NULL;
    }
    feeder_slots[i].play_frame = 0;
    g_atomic_int_set(&feeder_slots[i].locate_req, 0);
}

/* Write n frames of feeder_L/feeder_R into a track's playback ringbuffers. */
static void feeder_write(JackDawTrack *t, size_t n)
{
    if (n == 0)
        return;
    rt_ringbuffer_write(t->play_buf_L, (const char *)feeder_L, n * sizeof(float));
    rt_ringbuffer_write(t->play_buf_R, (const char *)feeder_R, n * sizeof(float));
}

static void feeder_emit_silence(JackDawTrack *t, size_t n)
{
    memset(feeder_L, 0, n * sizeof(float));
    memset(feeder_R, 0, n * sizeof(float));
    feeder_write(t, n);
}

/* Loop range read off the writer's thread. start and end are two stores, so a
 * reader could pair a new start with an old end and wrap to the wrong place (a
 * seqlock around the pair closes that). The reader never waits for the writer:
 * an RT thread spinning on a preempted window thread could spin for a whole
 * scheduler slice. After a few tries it gives up and returns TRUE, and the
 * caller keeps the pair it read last time, so a new range lands at most a
 * cycle late but is never seen torn. */
typedef struct {
    off_t start, end;
} EngLoopRange;

static gboolean eng_loop_read(EngLoopRange *out)
{
    for (int tries = 0; tries < 4; tries++) {
        gint s0 = g_atomic_int_get(&engine.loop_seq);
        if (s0 & 1)
            continue;
        off_t a = __atomic_load_n(&engine.loop_start, __ATOMIC_RELAXED);
        off_t b = __atomic_load_n(&engine.loop_end, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE); /* pair loads before re-check */
        if (g_atomic_int_get(&engine.loop_seq) == s0) {
            out->start = a;
            out->end = b;
            return FALSE;
        }
    }
    return TRUE;
}

/* Block until every RT cycle that could have seen the state from before this
 * call has finished, so a port hidden from the callback (count lowered, slot
 * cleared, or pointer cleared) can be unregistered. Hiding it is not enough on
 * its own: a cycle already in flight loaded the old value and may still be
 * about to jack_port_get_buffer() the port. engine_cycles is bumped at the
 * START of each callback, so two increments prove the in-flight cycle is over.
 *
 * Bounded: if no cycle starts for ENG_RT_WAIT_US the callback is not being run
 * (client zombified or server stalled), so nothing is in flight and returning
 * is safe. Window threads only; costs at most about two periods. */
#define ENG_RT_WAIT_US (2 * G_USEC_PER_SEC)
static void eng_wait_rt_past(void)
{
    if (!engine.active || !engine.client)
        return;
    guint c0 = (guint)g_atomic_int_get(&engine_cycles);
    gint64 deadline = g_get_monotonic_time() + ENG_RT_WAIT_US;
    while ((guint)((guint)g_atomic_int_get(&engine_cycles) - c0) < 2u &&
           g_get_monotonic_time() < deadline)
        g_usleep(500);
}

static void *feeder_thread_func(void *arg)
{
    (void)arg;
    guint i;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000}; /* 2 ms sleep */
    EngLoopRange feed_loop = {0, 0};

    while (!feeder_stop_flag) {
        nanosleep(&ts, NULL);

        if (!(g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING))
            continue;
        if (!engine.client)
            continue;

        eng_loop_read(&feed_loop); /* one pair for the whole pass */

        int jack_sr = (int)jack_get_sample_rate(engine.client);

        for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            JackDawTrack *t = engine.slots[i];

            if (!t) {
                if (feeder_slots[i].snap || feeder_slots[i].sf)
                    feeder_slot_release(i);
                continue;
            }
            if (!t->play_buf_L || !t->play_buf_R)
                continue;

            /* --- Refresh the immutable region snapshot --- */
            ClipRegionSnapshot *ns = jackdaw_track_ref_snapshot(t);
            if (ns != feeder_slots[i].snap) {
                feeder_slot_close(i);
                if (feeder_slots[i].snap)
                    clip_region_snapshot_unref(feeder_slots[i].snap);
                feeder_slots[i].snap = ns;
            } else {
                clip_region_snapshot_unref(ns);
            }
            ClipRegionSnapshot *snap = feeder_slots[i].snap;

            /* --- Handle locate request from the main thread --- */
            if (g_atomic_int_compare_and_exchange(&feeder_slots[i].locate_req, 1, 0)) {
                feeder_slots[i].play_frame = feeder_slots[i].locate_frame;
                feeder_slot_close(i);
            }

            /* --- Inner fill loop: top up the ringbuffer each wakeup --- */
            while (TRUE) {
                size_t space_L = rt_ringbuffer_write_space(t->play_buf_L) / sizeof(float);
                size_t space_R = rt_ringbuffer_write_space(t->play_buf_R) / sizeof(float);
                size_t out_want = (space_L < space_R) ? space_L : space_R;
                if (out_want > FEEDER_CHUNK_FRAMES)
                    out_want = FEEDER_CHUNK_FRAMES;
                if (out_want == 0)
                    break; /* buffer full */

                off_t pf = feeder_slots[i].play_frame;

                /* --- Loop wrap: keep the feeder's stream inside the loop region
                 * so the ringbuffer never buffers audio past loop_end. --- */
                if (g_atomic_int_get(&engine.loop_enabled)) {
                    off_t l_start = feed_loop.start;
                    off_t l_end = feed_loop.end;
                    if (l_end > l_start) {
                        if (pf == l_end) {
                            pf = l_start;
                            feeder_slots[i].play_frame = pf;
                            feeder_slot_close(i); /* force re-seek of source */
                        }
                        if (pf < l_start) {
                            off_t room = l_start - pf; /* stop at region start */
                            if ((off_t)out_want > room)
                                out_want = (size_t)room;
                        } else if (pf < l_end) {
                            off_t room = l_end - pf; /* stop at region end */
                            if ((off_t)out_want > room)
                                out_want = (size_t)room;
                        }
                    }
                }

                /* Locate the region covering pf and the next region after it. */
                ClipRegion *reg = NULL;
                int reg_idx = -1;
                off_t next_start = -1;
                for (int k = 0; snap && k < snap->n; k++) {
                    ClipRegion *r = &snap->r[k];
                    if (pf >= r->tl_pos && pf < r->tl_pos + r->length) {
                        reg = r;
                        reg_idx = k;
                        break;
                    }
                    if (r->tl_pos > pf && (next_start < 0 || r->tl_pos < next_start))
                        next_start = r->tl_pos;
                }

                /* --- Gap / before first / past end: emit silence --- */
                if (!reg) {
                    size_t sil = out_want;
                    if (next_start >= 0) {
                        off_t to_next = next_start - pf;
                        if (to_next > 0 && (off_t)sil > to_next)
                            sil = (size_t)to_next;
                    }
                    if (sil == 0)
                        break;
                    if (feeder_slots[i].sf)
                        feeder_slot_close(i);
                    feeder_emit_silence(t, sil);
                    feeder_slots[i].play_frame = pf + (off_t)sil;
                    continue;
                }

                int clip_sr = reg->clip ? reg->clip->info.samplerate : jack_sr;
                int clip_ch = reg->clip ? reg->clip->info.channels : 1;
                int eff_ch = (clip_ch > FEEDER_MAX_CHANNELS) ? FEEDER_MAX_CHANNELS : clip_ch;
                gboolean needs_src = (clip_sr != jack_sr);
                off_t d = pf - reg->tl_pos;         /* timeline frames in */
                off_t reg_remain = reg->length - d; /* timeline frames left */
                if (reg_remain <= 0) {
                    feeder_slots[i].play_frame = reg->tl_pos + reg->length;
                    continue;
                }

                /* --- Open / seek file for this region if needed --- */
                if (reg_idx != feeder_slots[i].open_region || !feeder_slots[i].sf) {
                    feeder_slot_close(i);
                    SF_INFO sfi = {0};
                    SNDFILE *sf = reg->clip ? sf_open(reg->clip->path, SFM_READ, &sfi) : NULL;
                    if (!sf) {
                        /* Cannot read — render the rest of the region as silence */
                        size_t sil = out_want;
                        if ((off_t)sil > reg_remain)
                            sil = (size_t)reg_remain;
                        feeder_emit_silence(t, sil);
                        feeder_slots[i].play_frame = pf + (off_t)sil;
                        continue;
                    }
                    off_t file_off =
                        reg->file_in +
                        ((clip_sr == jack_sr) ? d : (off_t)((double)d * clip_sr / jack_sr + 0.5));
                    sf_seek(sf, file_off, SEEK_SET);
                    feeder_slots[i].sf = sf;
                    feeder_slots[i].open_clip_sr = clip_sr;
                    feeder_slots[i].open_clip_ch = clip_ch;
                    feeder_slots[i].open_region = reg_idx;
                    if (needs_src) {
                        int e = 0;
                        feeder_slots[i].src_L = src_new(SRC_SINC_FASTEST, 1, &e);
                        if (eff_ch > 1)
                            feeder_slots[i].src_R = src_new(SRC_SINC_FASTEST, 1, &e);
                    }
                }

                size_t want = out_want;
                if ((off_t)want > reg_remain)
                    want = (size_t)reg_remain;
                gfloat gain = reg->gain;

                if (!needs_src) {
                    /* ---- Direct copy path ---- */
                    sf_count_t got =
                        sf_readf_float(feeder_slots[i].sf, feeder_raw, (sf_count_t)want);
                    if (got < 0)
                        got = 0;
                    if (eff_ch == 1) {
                        for (sf_count_t f = 0; f < got; f++)
                            feeder_L[f] = feeder_R[f] = feeder_raw[f] * gain;
                    } else {
                        for (sf_count_t f = 0; f < got; f++) {
                            feeder_L[f] = feeder_raw[f * eff_ch] * gain;
                            feeder_R[f] = feeder_raw[f * eff_ch + 1] * gain;
                        }
                    }
                    if ((size_t)got < want) {
                        size_t pad = want - (size_t)got;
                        memset(feeder_L + got, 0, pad * sizeof(float));
                        memset(feeder_R + got, 0, pad * sizeof(float));
                    }
                    feeder_write(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;
                } else if (feeder_slots[i].src_L) {
                    /* ---- Resampled path (clip SR != JACK SR) ---- */
                    double ratio = (double)jack_sr / (double)clip_sr;
                    long want_l = (long)want;
                    long in_need = (long)ceil((double)want / ratio) + 8;
                    if (in_need > FEEDER_RAW_FRAMES)
                        in_need = FEEDER_RAW_FRAMES;
                    int eoi = (want == (size_t)reg_remain);

                    sf_count_t got =
                        sf_readf_float(feeder_slots[i].sf, feeder_raw, (sf_count_t)in_need);
                    if (got < 0)
                        got = 0;

                    for (sf_count_t f = 0; f < got; f++)
                        feeder_mono[f] = feeder_raw[f * eff_ch];
                    SRC_DATA sd_L = {.data_in = feeder_mono,
                                     .data_out = feeder_L,
                                     .input_frames = (long)got,
                                     .output_frames = want_l,
                                     .src_ratio = ratio,
                                     .end_of_input = eoi};
                    src_process(feeder_slots[i].src_L, &sd_L);
                    long out_gen = sd_L.output_frames_gen;

                    if (eff_ch > 1 && feeder_slots[i].src_R) {
                        for (sf_count_t f = 0; f < got; f++)
                            feeder_mono[f] = feeder_raw[f * eff_ch + 1];
                        SRC_DATA sd_R = {.data_in = feeder_mono,
                                         .data_out = feeder_R,
                                         .input_frames = (long)got,
                                         .output_frames = want_l,
                                         .src_ratio = ratio,
                                         .end_of_input = eoi};
                        src_process(feeder_slots[i].src_R, &sd_R);
                    } else if (out_gen > 0) {
                        memcpy(feeder_R, feeder_L, (size_t)out_gen * sizeof(float));
                    }

                    for (long k = 0; k < out_gen; k++) {
                        feeder_L[k] *= gain;
                        feeder_R[k] *= gain;
                    }
                    if (out_gen < want_l) {
                        size_t pad = (size_t)(want_l - out_gen);
                        memset(feeder_L + out_gen, 0, pad * sizeof(float));
                        memset(feeder_R + out_gen, 0, pad * sizeof(float));
                    }
                    feeder_write(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;

                    /* Rewind file input SRC did not consume so the next read
                     * stays aligned with the timeline. */
                    long used = sd_L.input_frames_used;
                    if (used < got)
                        sf_seek(feeder_slots[i].sf, -(sf_count_t)(got - used), SEEK_CUR);
                } else {
                    /* SRC needed but its allocation failed — silence the region. */
                    feeder_emit_silence(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;
                }
            } /* inner fill loop */
        } /* for each slot */
    } /* while !feeder_stop_flag */

    /* Clean up all open handles and snapshots on thread exit */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++)
        feeder_slot_release(i);

    return NULL;
}

static void feeder_start(void)
{
    if (feeder_started)
        return;

    feeder_raw = g_new(float, FEEDER_RAW_FRAMES *FEEDER_MAX_CHANNELS);
    feeder_mono = g_new(float, FEEDER_RAW_FRAMES);
    feeder_L = g_new(float, FEEDER_CHUNK_FRAMES);
    feeder_R = g_new(float, FEEDER_CHUNK_FRAMES);

    feeder_stop_flag = 0;
    memset(feeder_slots, 0, sizeof(feeder_slots));
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
        feeder_slots[i].open_region = -1;

    if (pthread_create(&feeder_tid, NULL, feeder_thread_func, NULL) != 0) {
        g_free(feeder_raw);
        g_free(feeder_mono);
        g_free(feeder_L);
        g_free(feeder_R);
        feeder_raw = feeder_mono = feeder_L = feeder_R = NULL;
        return;
    }
    feeder_started = TRUE;
}

static void feeder_stop(void)
{
    if (!feeder_started)
        return;
    feeder_stop_flag = 1;
    pthread_join(feeder_tid, NULL);
    feeder_started = FALSE;
    g_free(feeder_raw);
    feeder_raw = NULL;
    g_free(feeder_mono);
    feeder_mono = NULL;
    g_free(feeder_L);
    feeder_L = NULL;
    g_free(feeder_R);
    feeder_R = NULL;
}

/* -----------------------------------------------------------------------
 * Recorder thread
 *
 * A dedicated non-RT pthread drains each armed track's rec_buf_L/R capture
 * ringbuffers into a 24-bit WAV on disk (write-through so a take is never lost)
 * and, when a take stops, hands the finished file to the main thread to place
 * as a region. Like the feeder it may allocate, open files and do disk I/O; the
 * only contract it shares with the RT callback is the lock-free ringbuffer.
 * Ported from the Linux JackDAW engine; the g_idle_add main-thread hand-off is
 * replaced by a GAsyncQueue drained on the window looper (Haiku has no GLib main
 * loop), signalled via the engine event hook.
 * ----------------------------------------------------------------------- */

#define REC_SCRATCH_FRAMES 4096
/* Max JACK periods buffered for the live waveform (~46 min at 48k/1024). */
#define REC_PEAK_MAX_BUCKETS 131072

typedef struct {
    SNDFILE *sf;                    /* open for writing; NULL = idle */
    char path[512];                 /* destination file path */
    off_t written;                  /* frames written so far */
    volatile off_t expected_frames; /* frames to capture; 0 = open-ended, set at stop */
    gint finalize_req;              /* g_atomic: stop signals 1, recorder drains + closes */
    int channels;                   /* 1 = mono, 2 = stereo; set when the file is opened */
    int punch;                      /* 1 = punch take; overwrites a region on finalize */
    off_t punch_tl_start;           /* loop_start at arm time (timeline frames) */
    off_t punch_tl_end;             /* loop_end   at arm time (timeline frames) */
} RecorderSlot;

static RecorderSlot recorder_slots[JACKDAW_MAX_TRACKS];
static pthread_t recorder_tid;
static volatile int recorder_stop_flag;
static gboolean recorder_started = FALSE;

static float *rec_scratch_L;
static float *rec_scratch_R;
static float *rec_interleaved; /* 2 * REC_SCRATCH_FRAMES for the stereo write */

/* Finished takes waiting to be placed on the timeline. The recorder thread
 * pushes; the window looper pops in jackdaw_engine_finalize_takes(). */
typedef struct {
    JackDawTrack *track; /* strong ref — released when placed */
    char path[512];
    int punch; /* 1 = overwrite [punch_tl_start, punch_tl_end) */
    off_t punch_tl_start;
    off_t punch_tl_end;
} RecordFinalize;

static GAsyncQueue *recorder_done_q; /* of RecordFinalize*; created at recorder_start */

/* Defined with the UI event hook below; posts a non-blocking event to the UI. */
static void engine_post_event(int event);

/* Drain a slot's rec_buf, close the WAV, and queue the finished take for the
 * main thread. Runs on the recorder thread only. */
static void recorder_slot_finalize(guint i)
{
    RecorderSlot *rs = &recorder_slots[i];
    if (!rs->sf)
        return;

    JackDawTrack *t = engine.slots[i];
    if (t && t->rec_buf_L && t->rec_buf_R) {
        while (TRUE) {
            size_t avL = rt_ringbuffer_read_space(t->rec_buf_L) / sizeof(float);
            size_t avR =
                (rs->channels == 1) ? avL : rt_ringbuffer_read_space(t->rec_buf_R) / sizeof(float);
            size_t av = avL < avR ? avL : avR;
            if (av > REC_SCRATCH_FRAMES)
                av = REC_SCRATCH_FRAMES;
            /* Cap at expected_frames so the WAV ends exactly at the stop point. */
            if (rs->expected_frames > 0) {
                off_t rem = rs->expected_frames - rs->written;
                if (rem <= 0)
                    break;
                if ((off_t)av > rem)
                    av = (size_t)rem;
            }
            if (av == 0)
                break;

            rt_ringbuffer_read(t->rec_buf_L, (char *)rec_scratch_L, av * sizeof(float));
            if (rs->channels == 1) {
                rs->written += sf_writef_float(rs->sf, rec_scratch_L, (sf_count_t)av);
            } else {
                rt_ringbuffer_read(t->rec_buf_R, (char *)rec_scratch_R, av * sizeof(float));
                for (size_t f = 0; f < av; f++) {
                    rec_interleaved[f * 2] = rec_scratch_L[f];
                    rec_interleaved[f * 2 + 1] = rec_scratch_R[f];
                }
                rs->written += sf_writef_float(rs->sf, rec_interleaved, (sf_count_t)av);
            }
        }
    }

    sf_close(rs->sf);
    rs->sf = NULL;

    if (t && rs->written > 0 && recorder_done_q) {
        RecordFinalize *rf = g_new0(RecordFinalize, 1);
        rf->track = g_object_ref(t);
        g_strlcpy(rf->path, rs->path, sizeof(rf->path));
        rf->punch = rs->punch;
        rf->punch_tl_start = rs->punch_tl_start;
        /* Overwrite exactly the span captured: on an early stop (punch-out never
         * reached) only the recorded portion is replaced. */
        rf->punch_tl_end = rs->punch_tl_start + rs->written;
        g_async_queue_push(recorder_done_q, rf);
        engine_post_event(JACKDAW_ENGINE_EVENT_TAKE_FINALIZED);
    }
    rs->written = 0;
    rs->punch = 0;
}

static void *recorder_thread_func(void *arg)
{
    (void)arg;
    guint i;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000}; /* 2 ms */

    while (!recorder_stop_flag) {
        nanosleep(&ts, NULL);

        for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            /* Main thread signals finalize when a take stops. */
            if (g_atomic_int_compare_and_exchange(&recorder_slots[i].finalize_req, 1, 0)) {
                recorder_slot_finalize(i);
                continue;
            }

            RecorderSlot *rs = &recorder_slots[i];
            if (!rs->sf)
                continue;

            JackDawTrack *t = engine.slots[i];
            if (!t || !t->rec_buf_L || !t->rec_buf_R)
                continue;

            while (TRUE) {
                size_t avL = rt_ringbuffer_read_space(t->rec_buf_L) / sizeof(float);
                size_t avR = (rs->channels == 1)
                                 ? avL
                                 : rt_ringbuffer_read_space(t->rec_buf_R) / sizeof(float);
                size_t av = avL < avR ? avL : avR;
                if (av > REC_SCRATCH_FRAMES)
                    av = REC_SCRATCH_FRAMES;
                if (rs->expected_frames > 0) {
                    off_t rem = rs->expected_frames - rs->written;
                    if (rem <= 0)
                        break;
                    if ((off_t)av > rem)
                        av = (size_t)rem;
                }
                if (av == 0)
                    break;

                rt_ringbuffer_read(t->rec_buf_L, (char *)rec_scratch_L, av * sizeof(float));
                if (rs->channels == 1) {
                    rs->written += sf_writef_float(rs->sf, rec_scratch_L, (sf_count_t)av);
                } else {
                    rt_ringbuffer_read(t->rec_buf_R, (char *)rec_scratch_R, av * sizeof(float));
                    for (size_t f = 0; f < av; f++) {
                        rec_interleaved[f * 2] = rec_scratch_L[f];
                        rec_interleaved[f * 2 + 1] = rec_scratch_R[f];
                    }
                    rs->written += sf_writef_float(rs->sf, rec_interleaved, (sf_count_t)av);
                }
            }
        }
    }

    /* On thread exit: close any still-open files (engine quit path). */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (recorder_slots[i].sf) {
            sf_close(recorder_slots[i].sf);
            recorder_slots[i].sf = NULL;
        }
    }
    return NULL;
}

static void recorder_start(void)
{
    if (recorder_started)
        return;

    rec_scratch_L = g_new(float, REC_SCRATCH_FRAMES);
    rec_scratch_R = g_new(float, REC_SCRATCH_FRAMES);
    rec_interleaved = g_new(float, REC_SCRATCH_FRAMES * 2);
    if (!recorder_done_q)
        recorder_done_q = g_async_queue_new();

    recorder_stop_flag = 0;
    memset(recorder_slots, 0, sizeof(recorder_slots));

    if (pthread_create(&recorder_tid, NULL, recorder_thread_func, NULL) != 0) {
        g_free(rec_scratch_L);
        g_free(rec_scratch_R);
        g_free(rec_interleaved);
        rec_scratch_L = rec_scratch_R = rec_interleaved = NULL;
        return;
    }
    recorder_started = TRUE;
}

static void recorder_stop(void)
{
    if (!recorder_started)
        return;
    recorder_stop_flag = 1;
    pthread_join(recorder_tid, NULL);
    recorder_started = FALSE;
    g_free(rec_scratch_L);
    rec_scratch_L = NULL;
    g_free(rec_scratch_R);
    rec_scratch_R = NULL;
    g_free(rec_interleaved);
    rec_interleaved = NULL;
}

/* ---- UI event hook (see header) ---- */

static JackDawEngineEventHook engine_event_hook = NULL;
static void *engine_event_hook_user = NULL;

void jackdaw_engine_set_event_hook(JackDawEngineEventHook hook, void *user)
{
    engine_event_hook = hook;
    engine_event_hook_user = user;
}

/* Called from JACK notification threads — must not block, allocate, touch UI
 * or emit GObject signals. The hook only posts a message. */
static void engine_post_event(int event)
{
    JackDawEngineEventHook hook = engine_event_hook;
    if (hook)
        hook(event, engine_event_hook_user);
}

/* -----------------------------------------------------------------------
 * Instrument-track MIDI scheduling / recording (RT)
 *
 * Each instrument track owns a midi_in[slot] (external source) and a
 * midi_out[slot] (to a synth/hardware). The RT callback merges, per slot and in
 * time order into midi_out[slot]: released note-offs from a stop/seek flush,
 * sequenced notes from the immutable rt_midi snapshot (while playing), live
 * input (thru, while armed) and preview notes queued from the main thread.
 * Per-slot active-note bookkeeping lets a stop/seek release sounding notes so no
 * note is left stuck. Instrument-track audio stays silent until the plugin phase
 * gives these tracks a synth — the notes are heard via midi_out.
 * ----------------------------------------------------------------------- */

#define ENG_MIDI_MAX_EV 1024
static guint8 eng_active_notes[JACKDAW_MAX_TRACKS][16][128];
static volatile gint eng_midi_flush[JACKDAW_MAX_TRACKS]; /* 1 = release all held notes */

/* One RT MIDI event: block-relative sample offset + up to 3 bytes. The
 * engine uses the plugin host's event type directly so a gathered block can
 * feed an instrument plugin and the midi_out port without conversion. */
typedef PhMidiEvent EngMidiEv;

/* Per-slot events gathered once per block in the track loop: consumed there
 * by the instrument plugin (first FX) and again by the midi_out writer below
 * — gathering twice would double-apply the flush/active-note bookkeeping. */
static EngMidiEv eng_block_ev[JACKDAW_MAX_TRACKS][ENG_MIDI_MAX_EV];
static int eng_block_nev[JACKDAW_MAX_TRACKS];

/* This cycle's control-input MIDI (the footswitch), for the effect plug-ins.
 * The same messages drive JackDAW's MIDI options through eng_control_rb, so one
 * press can mute a track and switch an amp plug-in's channel at once: a
 * plug-in with MIDI features of its own (MIDI learn, a footswitch) hears the
 * control input as a hardware amp would hear the pedal. Channel messages only.
 * Ownership: written by the JACK thread before it wakes the workers, then only
 * read by the workers (track chains) until the next cycle; the semaphore
 * post/wait that dispatches them orders the writes before their reads, as for
 * the g_rt_* slots. */
#define ENG_FX_MAX_EV 256
static EngMidiEv eng_fx_ev[ENG_FX_MAX_EV];
static int eng_fx_nev;

/* Preview-note injection (main thread -> RT). The main thread queues short
 * messages tagged with a track slot; the RT thread drains the ring once per
 * cycle into per-slot scratch, emitted at block offset 0. Lock-free SPSC. */
#define ENG_PREVIEW_MAX 32
typedef struct {
    gint32 slot;
    guint8 data[3];
} EngPrevMsg;
static rt_ringbuffer_t *eng_preview_rb; /* SPSC: main -> RT */
static guint8 eng_preview_data[JACKDAW_MAX_TRACKS][ENG_PREVIEW_MAX][3];
static int eng_preview_n[JACKDAW_MAX_TRACKS];

/* Control-surface events copied out of control_in by the RT callback and drained
 * on the main thread by jackdaw_engine_control_poll(). Lock-free SPSC RT->main. */
static rt_ringbuffer_t *eng_control_rb;

/* One recorded MIDI event: absolute timeline frame + up to 3 bytes. Written by
 * the RT thread to t->midi_rec_buf, drained on the main thread when recording
 * stops (jackdaw_engine_finalize_midi_takes) and turned into clip notes. */
typedef struct {
    gint64 frame;
    guint8 size;
    guint8 data[3];
} MidiRecEvent;
/* Transport frame at the last stop — closes notes still held when recording ends. */
static volatile off_t eng_midi_rec_cut;

static int eng_midi_cmp(const void *a, const void *b)
{
    const EngMidiEv *ea = a, *eb = b;
    if (ea->time < eb->time)
        return -1;
    if (ea->time > eb->time)
        return 1;
    return 0;
}

/* Build this block's MIDI events for one instrument track: stop/seek flush
 * note-offs, then sequenced events from the immutable snapshot (while playing),
 * then live JACK MIDI input (thru, while armed), then main-thread preview notes.
 * Tracks sounding notes per slot so a later flush can release them. Returns the
 * event count; the caller sorts and writes them to midi_out[slot]. */
static int eng_gather_instrument_midi(int slot, JackDawTrack *t, off_t blk_start,
                                      jack_nframes_t nframes, gboolean playing, gboolean armed,
                                      void *midi_buf, EngMidiEv *mev, int cap)
{
    int nev = 0;

    if (g_atomic_int_compare_and_exchange(&eng_midi_flush[slot], 1, 0)) {
        for (int ch = 0; ch < 16; ch++)
            for (int p = 0; p < 128; p++)
                if (eng_active_notes[slot][ch][p] && nev < cap) {
                    mev[nev].time = 0;
                    mev[nev].size = 3;
                    mev[nev].data[0] = (guint8)(0x80 | ch);
                    mev[nev].data[1] = (guint8)p;
                    mev[nev].data[2] = 0;
                    nev++;
                    eng_active_notes[slot][ch][p] = 0;
                }
    }

    if (playing) {
        MidiEventSnapshot *ms = g_atomic_pointer_get(&t->rt_midi);
        if (ms && ms->n) {
            off_t end = blk_start + nframes;
            guint lo = 0, hi = ms->n; /* lower_bound(blk_start) */
            while (lo < hi) {
                guint mid = (lo + hi) / 2;
                if (ms->ev[mid].frame < blk_start)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            for (guint e = lo; e < ms->n && ms->ev[e].frame < end && nev < cap; e++) {
                MidiSnapEvent *se = &ms->ev[e];
                mev[nev].time = (guint32)(se->frame - blk_start);
                mev[nev].size = 3;
                mev[nev].data[0] = se->s;
                mev[nev].data[1] = se->d1;
                mev[nev].data[2] = se->d2;
                int ch = se->s & 0x0F, p = se->d1 & 0x7F;
                if ((se->s & 0xF0) == 0x90 && se->d2 > 0)
                    eng_active_notes[slot][ch][p] = 1;
                else if ((se->s & 0xF0) == 0x80)
                    eng_active_notes[slot][ch][p] = 0;
                nev++;
            }
        }
    }

    if (armed && midi_buf) {
        void *mbuf = midi_buf;
        uint32_t mc = jack_midi_get_event_count(mbuf);
        for (uint32_t m = 0; m < mc && nev < cap; m++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, mbuf, m) != 0 || ev.size < 1)
                continue;
            /* Don't thru system realtime (0xF8..0xFF): with a single MIDI device
             * the default wiring loops midi_out back to the source device, and
             * echoing its own clock/active-sensing back at it can confuse its
             * tempo sync. Voice messages (the notes) pass through. */
            if (ev.buffer[0] >= 0xF8)
                continue;
            mev[nev].time = ev.time;
            mev[nev].size = (guint8)(ev.size > 3 ? 3 : ev.size);
            mev[nev].data[0] = ev.buffer[0];
            mev[nev].data[1] = ev.size > 1 ? ev.buffer[1] : 0;
            mev[nev].data[2] = ev.size > 2 ? ev.buffer[2] : 0;
            int st = ev.buffer[0] & 0xF0, ch = ev.buffer[0] & 0x0F, p = mev[nev].data[1] & 0x7F;
            if (st == 0x90 && mev[nev].data[2] > 0)
                eng_active_notes[slot][ch][p] = 1;
            else if (st == 0x80)
                eng_active_notes[slot][ch][p] = 0;
            nev++;
        }
    }

    /* Preview notes queued from the main thread (piano-roll keyboard) — emitted
     * at block start and tracked so a later flush releases them. */
    if (slot >= 0 && slot < JACKDAW_MAX_TRACKS) {
        for (int pi = 0; pi < eng_preview_n[slot] && nev < cap; pi++) {
            guint8 *d = eng_preview_data[slot][pi];
            mev[nev].time = 0;
            mev[nev].size = 3;
            mev[nev].data[0] = d[0];
            mev[nev].data[1] = d[1];
            mev[nev].data[2] = d[2];
            int st = d[0] & 0xF0, ch = d[0] & 0x0F, p = d[1] & 0x7F;
            if (st == 0x90 && d[2] > 0)
                eng_active_notes[slot][ch][p] = 1;
            else if (st == 0x80)
                eng_active_notes[slot][ch][p] = 0;
            nev++;
        }
    }

    if (nev > 1)
        qsort(mev, nev, sizeof(EngMidiEv), eng_midi_cmp);
    return nev;
}

/* -----------------------------------------------------------------------
 * RT callbacks
 * ----------------------------------------------------------------------- */

static inline void engine_rt_set_denormal_mode(void)
{
    rt_set_denormal_mode();
}

/* JACK thread-init callback: runs once per RT thread JACK spawns, before any
 * process cycle. */
static void engine_thread_init_cb(void *arg)
{
    (void)arg;
    engine_rt_set_denormal_mode();
}

/* -----------------------------------------------------------------------
 * Parallel track processing (ported from the Linux JackDAW engine)
 *
 * Track FX chains are independent of each other; only the master sum depends on
 * all of them. So each cycle the JACK RT thread fans the tracks out across a
 * pool of RT-priority worker threads (work-stealing), joins on a barrier, then
 * sums each track's post-fader output into the master bus. This lets two heavy
 * amp-sims on two tracks run on two cores at once instead of serially on one.
 *
 * RT-safety: workers only ever touch ONE track's state at a time (its own
 * ringbuffers, plugins, scratch and peak fields — never shared), so no locks
 * are needed beyond the per-cycle go/done semaphores. jack_port_get_buffer()
 * must be called only on the JACK thread, so the process callback pre-fetches
 * each armed track's input port buffers into g_slot_* below and the workers use
 * the cached pointers. Denormal flush is re-armed per plugin in the host.
 * ----------------------------------------------------------------------- */
#define RT_MAX_WORKERS 64
static jack_native_thread_t g_rt_worker[RT_MAX_WORKERS];
static int g_rt_nworkers = 0; /* 0 = serial (RT thread does all tracks) */
static sem_t g_rt_sem_go;     /* main posts N; each worker waits one */
static sem_t g_rt_sem_done;   /* each worker posts one; main waits N */
static volatile gint g_rt_workers_quit = 0;

/* Cycle parameters published to the workers (set before the barrier opens; the
 * semaphores provide the memory barrier). */
static volatile jack_nframes_t g_rt_nframes;
static volatile gint g_rt_flags;
static volatile off_t g_rt_blk_start;
static volatile gint g_rt_any_soloed;
static volatile gint g_rt_task_next; /* work-stealing index into g_rt_task_slot */
/* Compact list of the slots that actually hold a track this cycle, rebuilt on
 * the JACK thread before the barrier opens. The work-stealing loop iterates
 * this rather than all JACKDAW_MAX_TRACKS slots: the shared counter is a
 * contended atomic, so scanning 64 slots to find one track made every worker
 * hammer the same cache line ~64 times per cycle for no work. */
static int g_rt_task_slot[JACKDAW_MAX_TRACKS];
static volatile gint g_rt_task_count;

/* Port buffers pre-fetched on the JACK thread (only it may call
 * jack_port_get_buffer); workers read these cached pointers. */
static float *g_slot_live_L[JACKDAW_MAX_TRACKS];
static float *g_slot_live_R[JACKDAW_MAX_TRACKS];
static void *g_slot_midi_buf[JACKDAW_MAX_TRACKS];

/* Process one track fully (drain/instrument, live monitor, FX chain, fader/pan,
 * metering, capture) into engine.slot_L[i]/slot_R[i]. Reads cycle params from
 * the g_rt_* publish slots and input buffers from g_slot_*. Runs on the JACK
 * thread or a worker; never calls jack_port_get_buffer. */
static void engine_process_track(int i)
{
    JackDawTrack *t = engine.slots[i];
    if (!t)
        return;

    jack_nframes_t nframes = g_rt_nframes;
    gint flags = g_rt_flags;
    off_t blk_start = g_rt_blk_start;
    gboolean any_soloed = g_rt_any_soloed != 0;
    jack_nframes_t k;
    size_t want = nframes * sizeof(float);

    float *bL = engine.slot_L[i];
    float *bR = engine.slot_R[i];
    gboolean instr = jackdaw_track_is_instrument(t);

    /* Clip playback: drain the feeder's ringbuffers for this track. An
     * instrument track has no audio regions; a stopped transport plays nothing.
     * Short reads (feeder underrun) are zero-padded. Live input monitoring sums
     * on top below. */
    if (!instr && t->play_buf_L && t->play_buf_R && (flags & ENGINE_PLAYING)) {
        size_t got_L = rt_ringbuffer_read(t->play_buf_L, (char *)bL, want);
        size_t got_R = rt_ringbuffer_read(t->play_buf_R, (char *)bR, want);
        if (got_L < want)
            memset((char *)bL + got_L, 0, want - got_L);
        if (got_R < want)
            memset((char *)bR + got_R, 0, want - got_R);
    } else {
        memset(bL, 0, want);
        memset(bR, 0, want);
    }

    gint tflags = g_atomic_int_get(&t->state_flags);
    gboolean muted = (tflags & TRACK_MUTED) || (any_soloed && !(tflags & TRACK_SOLOED));

    /* Live input monitor: armed audio track with an assigned capture port. The
     * port buffer was pre-fetched on the JACK thread. A mono track duplicates
     * its one input to both channels. */
    if (!instr && (tflags & TRACK_ARMED) && g_slot_live_L[i]) {
        float *live_L = g_slot_live_L[i];
        float *live_R = g_slot_live_R[i] ? g_slot_live_R[i] : live_L;
        gboolean recording = (flags & ENGINE_RECORDING) != 0;
        /* While recording, an armed track monitors ONLY its live input (the take
         * being cut), not clip playback — clear the drained playback so the two
         * don't sum. */
        if (recording) {
            memset(bL, 0, want);
            memset(bR, 0, want);
        }
        float mn = 0.0f, mx = 0.0f;
        for (k = 0; k < nframes; k++) {
            float sl = live_L[k], sr = live_R[k];
            bL[k] += sl;
            bR[k] += sr;
            if (sl < mn)
                mn = sl;
            if (sl > mx)
                mx = sl;
            if (sr < mn)
                mn = sr;
            if (sr > mx)
                mx = sr;
        }
        /* Capture the dry input to the record ringbuffers (write-through to disk
         * via the recorder thread) and append a live-waveform min/max bucket for
         * the red overlay. A mono take writes only the L ring. */
        if (recording) {
            if (t->rec_buf_L)
                rt_ringbuffer_write(t->rec_buf_L, (const char *)live_L, want);
            if (t->stereo_input && t->rec_buf_R)
                rt_ringbuffer_write(t->rec_buf_R, (const char *)live_R, want);
            gint pk = t->rec_peak_count;
            if (t->rec_peak_buf && pk < REC_PEAK_MAX_BUCKETS) {
                t->rec_peak_buf[pk * 2] = mn;
                t->rec_peak_buf[pk * 2 + 1] = mx;
                t->rec_peak_count = pk + 1;
            }
        }
    }

    /* MIDI-path diagnostics (JACKDAW_DIAG): count raw events on this instrument
     * track's midi_in BEFORE any arm/record gate, and expose the gate state, so
     * a dead port read can be told apart from a wrong gate. Counters are atomic:
     * several workers may touch them in the same cycle. */
    if (g_diag_on && instr && g_slot_midi_buf[i]) {
        g_atomic_int_add(&g_diag_midi_in, (gint)jack_midi_get_event_count(g_slot_midi_buf[i]));
        g_atomic_int_set(&g_diag_instr_state,
                         ((tflags & TRACK_ARMED) ? 1 : 0) | ((flags & ENGINE_RECORDING) ? 2 : 0) |
                             ((flags & ENGINE_PLAYING) ? 4 : 0) | (t->midi_rec_buf ? 8 : 0));
    }

    /* MIDI record: capture an armed instrument track's live input events with
     * absolute timeline frames. Voice/common messages only — system realtime
     * (0xF8..0xFF) is dropped so a drum module's continuous clock can't fill the
     * fixed capture ring mid-take. Drained into notes on the main thread at stop. */
    if (instr && (tflags & TRACK_ARMED) && (flags & ENGINE_RECORDING) && g_slot_midi_buf[i] &&
        t->midi_rec_buf) {
        void *mbuf = g_slot_midi_buf[i];
        uint32_t mc = jack_midi_get_event_count(mbuf);
        for (uint32_t m = 0; m < mc; m++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, mbuf, m) != 0 || ev.size < 1)
                continue;
            if (!(ev.buffer[0] & 0x80) || ev.buffer[0] >= 0xF8)
                continue;
            MidiRecEvent r;
            r.frame = (gint64)(blk_start + (off_t)ev.time);
            r.size = (guint8)(ev.size > 3 ? 3 : ev.size);
            r.data[0] = ev.buffer[0];
            r.data[1] = ev.size > 1 ? ev.buffer[1] : 0;
            r.data[2] = ev.size > 2 ? ev.buffer[2] : 0;
            if (rt_ringbuffer_write_space(t->midi_rec_buf) >= sizeof r) {
                rt_ringbuffer_write(t->midi_rec_buf, (const char *)&r, sizeof r);
                if (g_diag_on)
                    g_atomic_int_inc(&g_diag_midi_rec);
            }
        }
    }

    /* Instrument tracks: gather this block's MIDI ONCE (stop/seek flush,
     * sequenced snapshot, live thru, preview notes) into per-slot scratch. It
     * feeds the instrument plugin below AND the midi_out writer after the barrier
     * — gathering twice would double-apply the flush and active-note bookkeeping. */
    if (instr)
        eng_block_nev[i] = eng_gather_instrument_midi(
            (int)i, t, blk_start, nframes, (flags & ENGINE_PLAYING) != 0,
            (tflags & TRACK_ARMED) != 0, g_slot_midi_buf[i], eng_block_ev[i], ENG_MIDI_MAX_EV);

    /* Per-track FX chain, in place on bL/bR (pre-fader). On an instrument track
     * the FIRST plugin is the instrument: it receives the block's MIDI and
     * renders audio into the (silent) track buffers; the rest process it, and
     * hear the control input rather than the track's notes (see eng_fx_ev). */
    JackDawFxChain *chain = g_atomic_pointer_get(&t->rt_chain);
    if (chain) {
        int fi = 0;
        if (instr && chain->n > 0) {
            pluginhost_process_midi((PluginInstance *)chain->fx[0], eng_block_ev[i],
                                    eng_block_nev[i], bL, bR, (int)nframes);
            fi = 1;
        }
        for (; fi < chain->n; fi++)
            pluginhost_process_fx_midi((PluginInstance *)chain->fx[fi], eng_fx_ev, eng_fx_nev, bL,
                                       bR, (int)nframes);
        /* Fold this cycle's per-plugin timings into the diag table. Reading
         * them here (on the RT thread that just produced them) keeps the
         * instance pointers valid without extra synchronisation. */
        if (g_diag_on) {
            for (int di = 0; di < chain->n; di++) {
                gint us = (gint)pluginhost_diag_take_max_us((PluginInstance *)chain->fx[di]);
                int slot = di < ENG_DIAG_FX_SLOTS ? di : ENG_DIAG_FX_SLOTS - 1;
                if (us > g_atomic_int_get(&g_diag_fx_us[i][slot]))
                    g_atomic_int_set(&g_diag_fx_us[i][slot], us);
            }
        }
    }

    /* Constant-power pan + effective fader, written in place into the slot; meter
     * post-fader with a decay hold so the VU falls back smoothly between peaks. */
    gfloat vol = muted ? 0.0f : t->volume;
    float angle = (t->pan + 1.0f) * (float)M_PI_4;
    float gain_L = vol * cosf(angle);
    float gain_R = vol * sinf(angle);
    /* Ramp from last cycle's gain. A mute used to cut at the period boundary,
     * which clicked; Switch Group swapping one pedal track for another is now a
     * one-period crossfade instead. */
    float g_L = engine.slot_gain_L[i], g_R = engine.slot_gain_R[i];
    float inv_n = nframes ? 1.0f / (float)nframes : 0.0f;
    float dg_L = (gain_L - g_L) * inv_n, dg_R = (gain_R - g_R) * inv_n;
    engine.slot_gain_L[i] = gain_L;
    engine.slot_gain_R[i] = gain_R;
    float peak_L = 0.0f, peak_R = 0.0f;
    for (k = 0; k < nframes; k++) {
        g_L += dg_L;
        g_R += dg_R;
        float sL = bL[k] * g_L;
        float sR = bR[k] * g_R;
        bL[k] = sL;
        bR[k] = sR;
        float aL = fabsf(sL), aR = fabsf(sR);
        if (aL > peak_L)
            peak_L = aL;
        if (aR > peak_R)
            peak_R = aR;
    }
    t->peak_L = (peak_L > t->peak_L) ? peak_L : t->peak_L * 0.92f;
    t->peak_R = (peak_R > t->peak_R) ? peak_R : t->peak_R * 0.92f;
}

/* Work-stealing runner: claim the next slot index and process it until none
 * remain. Called by both the JACK thread and every worker. */
static void rt_run_tasks(void)
{
    const int n = g_atomic_int_get(&g_rt_task_count);
    int idx;
    while ((idx = g_atomic_int_add(&g_rt_task_next, 1)) < n)
        engine_process_track(g_rt_task_slot[idx]);
}

static void rt_sem_wait(sem_t *s)
{
    while (sem_wait(s) != 0 && errno == EINTR) { /* retry */
    }
}

static void *rt_worker_main(void *arg)
{
    (void)arg;
    rt_set_denormal_mode();
    for (;;) {
        rt_sem_wait(&g_rt_sem_go);
        if (g_atomic_int_get(&g_rt_workers_quit))
            break;
        rt_run_tasks();
        sem_post(&g_rt_sem_done);
    }
    return NULL;
}

static int engine_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    guint i, oi;
    gint flags;
    float *port_buf;
    jack_nframes_t k;

    /* Diagnostics: time the whole cycle (JACKDAW_DIAG only). */
    gint64 diag_t0 = 0;
    if (g_diag_on)
        diag_t0 = g_get_monotonic_time();

    /* Flush denormals to zero on this RT thread (belt; thread-init cb is the
     * suspenders). */
    engine_rt_set_denormal_mode();

    /* Mark this thread RT for the plugin host's allocation diagnostics. */
    ph_rt_mark(1);

    /* One cycle done. Counted before any early-out below so that a suspended or
     * silent graph still advances it — the point is only "the RT thread has run
     * again since you looked". */
    g_atomic_int_inc(&engine_cycles);

    /* Offline render / graph-rebuild in progress: the render worker (or a
     * project load) owns the PluginInstances, so the live graph must touch none
     * of them. Output silence, freeze the transport, and return before any mix
     * or plugin work. */
    if (g_atomic_int_get(&engine.render_suspend)) {
        for (i = 0; i < engine.audio_out_count; i++) {
            if (!engine.audio_out[i])
                continue;
            port_buf = jack_port_get_buffer(engine.audio_out[i], nframes);
            memset(port_buf, 0, nframes * sizeof(float));
        }
        if (engine.midi_out) {
            for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
                if (engine.midi_out[i])
                    jack_midi_clear_buffer(jack_port_get_buffer(engine.midi_out[i], nframes));
            }
        }
        if (engine.metro_out) {
            port_buf = jack_port_get_buffer(engine.metro_out, nframes);
            memset(port_buf, 0, nframes * sizeof(float));
        }
        ph_rt_mark(0);
        return 0;
    }

    /* Clear master mix buffers */
    memset(engine.master_L, 0, nframes * sizeof(float));
    memset(engine.master_R, 0, nframes * sizeof(float));

    flags = g_atomic_int_get(&engine.transport_flags);

    /* Count-in pre-roll: a metronome-only lead-in. While active the project is
     * frozen (play_pos does not advance) and nothing records; the metronome
     * clicks from countin_pos. When the pre-roll length elapses the pending
     * transport (PLAYING, optionally RECORDING) engages and normal play begins
     * this block from the unchanged play_pos. Period-granular. */
    gboolean preroll = FALSE;
    off_t preroll_base = 0;
    if (g_atomic_int_get(&engine.countin_active)) {
        if (engine.countin_pos >= engine.countin_len) {
            gint pend = g_atomic_int_get(&engine.countin_pending_rec);
            g_atomic_int_set(&engine.countin_active, 0);
            g_atomic_int_or(&engine.transport_flags,
                            ENGINE_PLAYING | (pend ? ENGINE_RECORDING : 0));
            flags = g_atomic_int_get(&engine.transport_flags);
        } else {
            preroll = TRUE;
            preroll_base = engine.countin_pos;
            engine.countin_pos += (off_t)nframes;
        }
    }

    if (flags & ENGINE_PLAYING)
        engine.play_pos += nframes;

    /* Loop wrap (master clock — drives the metronome and the UI playhead, and
     * later MIDI scheduling and the feeder). Only engages when this block
     * started inside the region: block-start in [loop_start, loop_end). A
     * playhead placed after the region therefore plays straight through without
     * looping. The remainder past loop_end is carried over so the loop period
     * equals the region length. Checked at block granularity, so the loop point
     * quantizes to the JACK period (acceptable for now). */
    static EngLoopRange rt_loop; /* JACK thread only; kept if a read fails */
    eng_loop_read(&rt_loop);
    if ((flags & ENGINE_PLAYING) && g_atomic_int_get(&engine.loop_enabled)) {
        off_t l_start = rt_loop.start;
        off_t l_end = rt_loop.end;
        off_t bstart = engine.play_pos - (off_t)nframes;
        if (l_end > l_start && bstart >= l_start && bstart < l_end && engine.play_pos >= l_end)
            engine.play_pos = l_start + (engine.play_pos - l_end);
    }

    /* Punch in/out (independent of looping): auto-engage recording over the tab
     * region [loop_start, loop_end) while a punch is armed. Same block-granular
     * crossing test as the loop wrap; punch_armed gates it so normal recording
     * is unaffected. The local `flags` is updated in step so this block's capture
     * pass acts on it. The capture slots were pre-opened in start_playback, so no
     * file I/O happens here. */
    if ((flags & ENGINE_PLAYING) && g_atomic_int_get(&engine.punch_armed)) {
        off_t ls = rt_loop.start;
        off_t le = rt_loop.end;
        off_t bstart = engine.play_pos - (off_t)nframes;
        if (le > ls) {
            if (!(flags & ENGINE_RECORDING)) {
                if (engine.play_pos > ls && bstart < le) { /* crossed into region */
                    g_atomic_int_or(&engine.transport_flags, ENGINE_RECORDING);
                    flags |= ENGINE_RECORDING;
                }
            } else if (engine.play_pos >= le) { /* reached region end */
                g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);
                flags &= ~ENGINE_RECORDING;
                for (guint p = 0; p < JACKDAW_MAX_TRACKS; p++)
                    if (recorder_slots[p].sf && recorder_slots[p].punch)
                        g_atomic_int_set(&recorder_slots[p].finalize_req, 1);
                g_atomic_int_set(&engine.punch_armed, 0);
            }
        }
    }

    /* Per-track mix pass (single-threaded; the work-stealing pool is a later
     * optimisation — see the plan's flagged assumptions). For each active track:
     * pull its live input (armed audio tracks with a capture port), apply the
     * constant-power pan + effective fader, meter post-fader, and sum into the
     * master. Clip playback (regions) and FX chains fold in at their phases.
     *
     * Solo scan first: if any track is soloed, non-soloed tracks are muted. */

    /* Drain main-thread preview-note requests into per-slot scratch for this
     * cycle (reset counts first; each event is delivered exactly once). */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++)
        eng_preview_n[i] = 0;
    if (eng_preview_rb) {
        EngPrevMsg msg;
        while (rt_ringbuffer_read_space(eng_preview_rb) >= sizeof msg) {
            rt_ringbuffer_read(eng_preview_rb, (char *)&msg, sizeof msg);
            if (msg.slot >= 0 && msg.slot < JACKDAW_MAX_TRACKS &&
                eng_preview_n[msg.slot] < ENG_PREVIEW_MAX) {
                guint8 *d = eng_preview_data[msg.slot][eng_preview_n[msg.slot]++];
                d[0] = msg.data[0];
                d[1] = msg.data[1];
                d[2] = msg.data[2];
            }
        }
    }

    /* Copy the dedicated control-surface input into the RT->main ring; the main
     * thread interprets the mappings. RT side does only a bounded lock-free write
     * (drops on overflow) — no malloc/lock/log here. */
    eng_fx_nev = 0;
    if (engine.control_in && eng_control_rb) {
        void *cbuf = jack_port_get_buffer(engine.control_in, nframes);
        uint32_t cn = jack_midi_get_event_count(cbuf);
        for (uint32_t c = 0; c < cn; c++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, cbuf, c) != 0 || ev.size < 1)
                continue;
            JackDawCtlEvent cm;
            cm.size = (guint8)(ev.size > 3 ? 3 : ev.size);
            cm.data[0] = ev.buffer[0];
            cm.data[1] = ev.size > 1 ? ev.buffer[1] : 0;
            cm.data[2] = ev.size > 2 ? ev.buffer[2] : 0;
            if (rt_ringbuffer_write_space(eng_control_rb) >= sizeof cm)
                rt_ringbuffer_write(eng_control_rb, (const char *)&cm, sizeof cm);
            /* And to the effect plug-ins: channel messages (0x80-0xEF) whose
             * length matches their status; SysEx and clock are not theirs. */
            guint8 st = ev.buffer[0];
            size_t want = (st >= 0xC0 && st <= 0xDF) ? 2 : 3;
            if (st >= 0x80 && st <= 0xEF && ev.size == want && eng_fx_nev < ENG_FX_MAX_EV) {
                EngMidiEv *fe = &eng_fx_ev[eng_fx_nev++];
                fe->time = ev.time < nframes ? ev.time : nframes - 1;
                fe->size = (guint8)want;
                fe->data[0] = cm.data[0];
                fe->data[1] = cm.data[1];
                fe->data[2] = cm.data[2];
            }
        }
    }

    off_t blk_start = engine.play_pos - (off_t)nframes;

    /* Publish transport for plugins that query host time (tempo-synced
     * delays/LFOs, instrument arps) via the VST3 ProcessContext. */
    pluginhost_set_transport(engine.project ? engine.project->bpm : 120.0,
                             (double)engine.sample_rate, (gint64)blk_start,
                             (flags & ENGINE_PLAYING) != 0);

    gboolean any_soloed = FALSE;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (t && (g_atomic_int_get(&t->state_flags) & TRACK_SOLOED)) {
            any_soloed = TRUE;
            break;
        }
    }

    /* Pre-fetch each track's input port buffers on THIS (the JACK) thread —
     * jack_port_get_buffer must not be called from the workers — then fan the
     * tracks out across the worker pool (engine_process_track) and sum the
     * results. Also reset each slot's gathered-MIDI count so the midi_out writer
     * below sees 0 for tracks a worker never touched. */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        g_slot_live_L[i] = NULL;
        g_slot_live_R[i] = NULL;
        g_slot_midi_buf[i] = NULL;
        eng_block_nev[i] = 0;
        JackDawTrack *t = engine.slots[i];
        if (!t)
            continue;
        gint tflags = g_atomic_int_get(&t->state_flags);
        gboolean instr = jackdaw_track_is_instrument(t);
        if (!instr && (tflags & TRACK_ARMED) && t->audio_in_idx >= 0 &&
            (guint)t->audio_in_idx < engine.audio_in_count &&
            engine.audio_in[(guint)t->audio_in_idx]) {
            g_slot_live_L[i] =
                (float *)jack_port_get_buffer(engine.audio_in[(guint)t->audio_in_idx], nframes);
            /* Loaded once: a window thread clears it before unregistering. */
            jack_port_t *pr = g_atomic_pointer_get(&engine.audio_in_r[(guint)t->audio_in_idx]);
            if (t->audio_src_port_r && pr)
                g_slot_live_R[i] = (float *)jack_port_get_buffer(pr, nframes);
        }
        if (instr && t->midi_in_idx >= 0 && t->midi_in_idx < JACKDAW_MAX_TRACKS &&
            engine.midi_in[t->midi_in_idx])
            g_slot_midi_buf[i] = jack_port_get_buffer(engine.midi_in[t->midi_in_idx], nframes);
    }

    /* Publish cycle params and dispatch. The JACK thread participates as a worker
     * (work-stealing), so a single core still does all the work when no worker
     * threads are configured (g_rt_nworkers == 0). */
    g_rt_nframes = nframes;
    g_rt_flags = flags;
    g_rt_blk_start = blk_start;
    g_rt_any_soloed = any_soloed ? 1 : 0;

    /* Build this cycle's compact task list. */
    int ntasks = 0;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (engine.slots[i])
            g_rt_task_slot[ntasks++] = (int)i;
    }
    g_atomic_int_set(&g_rt_task_count, ntasks);
    g_atomic_int_set(&g_rt_task_next, 0);

    /* Wake only as many workers as there is parallel work for: the JACK thread
     * takes one track itself, so N tracks need at most N-1 helpers. Waking the
     * full pool for a one-track project cost two semaphore round-trips per
     * worker per cycle and had every woken thread contend for a task list it
     * would find empty — enough wall-clock to push a heavy plugin past its
     * deadline even though the CPU work was unchanged. */
    int wake = ntasks - 1;
    if (wake > g_rt_nworkers)
        wake = g_rt_nworkers;
    if (wake < 0)
        wake = 0;

    if (wake > 0) {
        for (int w = 0; w < wake; w++)
            sem_post(&g_rt_sem_go);
        rt_run_tasks();
        for (int w = 0; w < wake; w++)
            rt_sem_wait(&g_rt_sem_done);
    } else {
        rt_run_tasks();
    }

    /* Sum each processed track's post-fader contribution into the master. */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (!engine.slots[i])
            continue;
        const float *sl = engine.slot_L[i];
        const float *sr = engine.slot_R[i];
        for (k = 0; k < nframes; k++) {
            engine.master_L[k] += sl[k];
            engine.master_R[k] += sr[k];
        }
    }

    gfloat master_vol = engine.project ? engine.project->master_volume : 1.0f;
    if (engine.project && engine.project->master_muted)
        master_vol = 0.0f;
    float peak_L = 0.0f, peak_R = 0.0f;
    for (k = 0; k < nframes; k++) {
        engine.master_L[k] *= master_vol;
        engine.master_R[k] *= master_vol;
        float aL = fabsf(engine.master_L[k]);
        float aR = fabsf(engine.master_R[k]);
        if (aL > peak_L)
            peak_L = aL;
        if (aR > peak_R)
            peak_R = aR;
    }
    if (peak_L > engine.master_peak_L)
        engine.master_peak_L = peak_L;
    if (peak_R > engine.master_peak_R)
        engine.master_peak_R = peak_R;

    /* Realtime render tap: mirror the post-master-fader mix into the render ring
     * for the writer thread (master_L/R already carry the master fader here).
     * Capture only the frames of this block that fall before render_end so the
     * file ends exactly at the requested point; signal completion when the
     * playhead reaches it. */
    if (g_atomic_int_get(&engine.render_active) && engine.render_rb_L && engine.render_rb_R) {
        off_t blk_start = engine.play_pos - (off_t)nframes;
        off_t in_range = engine.render_end - blk_start;
        if (in_range < 0)
            in_range = 0;
        if (in_range > (off_t)nframes)
            in_range = (off_t)nframes;
        if (in_range > 0) {
            for (k = 0; k < (jack_nframes_t)in_range; k++) {
                engine.render_tap_L[k] = engine.master_L[k];
                engine.render_tap_R[k] = engine.master_R[k];
            }
            size_t bytes = (size_t)in_range * sizeof(float);
            if (rt_ringbuffer_write_space(engine.render_rb_L) >= bytes &&
                rt_ringbuffer_write_space(engine.render_rb_R) >= bytes) {
                rt_ringbuffer_write(engine.render_rb_L, (const char *)engine.render_tap_L, bytes);
                rt_ringbuffer_write(engine.render_rb_R, (const char *)engine.render_tap_R, bytes);
            }
        }
        if (engine.play_pos >= engine.render_end)
            g_atomic_int_set(&engine.render_done, 1);
    }

    /* Copy the master mix to the output ports */
    for (i = 0; i < engine.audio_out_count; i++) {
        if (!engine.audio_out[i])
            continue;
        port_buf = jack_port_get_buffer(engine.audio_out[i], nframes);
        memcpy(port_buf, (i == 0) ? engine.master_L : engine.master_R, nframes * sizeof(float));
    }

    /* Instrument-track MIDI output: clear every per-slot midi_out, then write
     * each instrument track's merged events (gathered once in the track loop
     * above, where they also fed the instrument plugin) to its own port, so
     * external hardware keeps hearing the notes alongside any plugin synth. */
    if (engine.midi_out) {
        for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            if (!engine.midi_out[i])
                continue;
            void *obuf = jack_port_get_buffer(engine.midi_out[i], nframes);
            jack_midi_clear_buffer(obuf);
            JackDawTrack *t = engine.slots[i];
            if (!t || !jackdaw_track_is_instrument(t))
                continue;
            for (int e = 0; e < eng_block_nev[i]; e++)
                jack_midi_event_write(obuf, eng_block_ev[i][e].time, eng_block_ev[i][e].data,
                                      eng_block_ev[i][e].size);
        }
    }

    /* Metronome click — monitored by mixing straight onto the audio outputs,
     * AFTER the master fader and the master peak meter. The click is therefore
     * audible but completely separate from the project signal: it cannot raise
     * the master meters and is independent of the master volume/mute.
     *
     * The dedicated "metronome" port always carries the click (a standalone feed
     * the user can route to a performer's headphones). metronome_route decides
     * whether the click ALSO bleeds into the main outs (MAIN) or stays on the
     * dedicated port only (CLICK_PORT = "headphones only"). The metro port is
     * cleared every cycle so it never plays stale buffer contents. */
    {
        float *metro_buf =
            engine.metro_out ? (float *)jack_port_get_buffer(engine.metro_out, nframes) : NULL;
        if (metro_buf)
            memset(metro_buf, 0, nframes * sizeof(float));

        /* The click sounds either for the normal metronome (playing + enabled) or
         * for a count-in pre-roll (always, regardless of the metronome toggle —
         * the count-in IS the click). The two cases differ only in the position
         * the beat grid is measured from. */
        gboolean play_click =
            (flags & ENGINE_PLAYING) && engine.project && engine.project->metronome_enabled;
        if ((play_click || preroll) && engine.project && engine.click_buf && engine.click_len > 0 &&
            engine.project->bpm > 0.0) {
            gboolean to_main = engine.project->metronome_route == METRONOME_ROUTE_MAIN;
            double fpb = (double)engine.sample_rate * 60.0 / engine.project->bpm;
            guint bpb = engine.project->beats_per_bar ? engine.project->beats_per_bar : 4;
            float click_gain = engine.project->metronome_gain;
            if (fpb > 1.0) {
                off_t base = preroll ? preroll_base : engine.play_pos - (off_t)nframes;
                float *out_buf[2] = {NULL, NULL};
                if (to_main) {
                    for (oi = 0; oi < engine.audio_out_count && oi < 2; oi++)
                        if (engine.audio_out[oi])
                            out_buf[oi] = jack_port_get_buffer(engine.audio_out[oi], nframes);
                }
                for (k = 0; k < nframes; k++) {
                    off_t a = base + (off_t)k;
                    if (a < 0)
                        continue;
                    /* The pre-roll sounds exactly `beats` clicks: the click at the
                     * resolution point (count-in position countin_len) belongs to
                     * the project's first downbeat, which the hand-off cycle plays
                     * from play_pos. Suppressing it here avoids a doubled click
                     * (a flam/pop) at the count-in -> transport transition. */
                    if (preroll && a >= engine.countin_len)
                        break;
                    off_t beat = (off_t)((double)a / fpb);
                    off_t boundary = (off_t)((double)beat * fpb + 0.5);
                    off_t off = a - boundary;
                    if (off >= 0 && off < engine.click_len) {
                        float s = engine.click_buf[off] * click_gain;
                        if ((beat % (off_t)bpb) != 0)
                            s *= 0.45f; /* accent downbeat */
                        if (metro_buf)
                            metro_buf[k] += s;
                        if (out_buf[0])
                            out_buf[0][k] += s;
                        if (out_buf[1])
                            out_buf[1][k] += s;
                    }
                }
            }
        }
    }

    if (diag_t0) {
        gint64 d = g_get_monotonic_time() - diag_t0;
        g_diag_cb_last_us = d;
        if (d > g_diag_cb_max_us)
            g_diag_cb_max_us = d;
    }

    ph_rt_mark(0);
    return 0;
}

/* -----------------------------------------------------------------------
 * Buffer size callback — called by JACK when buffer size changes.
 * Must NOT block; reallocates mix buffers.
 * ----------------------------------------------------------------------- */
static int engine_buffer_size_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    /* Reallocate mix scratch buffers. Re-allocation is allowed here: the
     * buffer-size callback runs outside the RT process cycle. */
    g_free(engine.master_L);
    g_free(engine.master_R);
    g_free(engine.render_tap_L);
    g_free(engine.render_tap_R);
    engine.master_L = g_malloc0(nframes * sizeof(float));
    engine.master_R = g_malloc0(nframes * sizeof(float));
    engine.render_tap_L = g_malloc0(nframes * sizeof(float));
    engine.render_tap_R = g_malloc0(nframes * sizeof(float));
    for (guint s = 0; s < JACKDAW_MAX_TRACKS; s++) {
        g_free(engine.slot_L[s]);
        g_free(engine.slot_R[s]);
        engine.slot_L[s] = g_malloc0(nframes * sizeof(float));
        engine.slot_R[s] = g_malloc0(nframes * sizeof(float));
    }
    engine.buf_size = nframes;

    return 0;
}

static void engine_shutdown_cb(void *arg)
{
    (void)arg;
    engine.active = FALSE;
    engine_post_event(JACKDAW_ENGINE_EVENT_SHUTDOWN);
}

static int engine_xrun_cb(void *arg)
{
    (void)arg;
    g_atomic_int_inc(&engine_xruns);
    return 0;
}

/* ---- Port topology callbacks (JACK notification thread → UI hook) ---- */

static void engine_port_reg_cb(jack_port_id_t port_id, int registered, void *arg)
{
    (void)port_id;
    (void)registered;
    (void)arg;
    engine_post_event(JACKDAW_ENGINE_EVENT_PORTS_CHANGED);
}

static void engine_port_connect_cb(jack_port_id_t a, jack_port_id_t b, int connect, void *arg)
{
    (void)a;
    (void)b;
    (void)connect;
    (void)arg;
    engine_post_event(JACKDAW_ENGINE_EVENT_CONNECTIONS_CHANGED);
}

/* Count physical ports of the given type/direction (>=1). Hardware capture
 * ports are JackPortIsOutput from a client's perspective (you connect from
 * them); playback ports are JackPortIsInput. */
static guint count_physical_ports(jack_client_t *c, const char *type, unsigned long flags)
{
    const char **ports = jack_get_ports(c, NULL, type, flags | JackPortIsPhysical);
    guint n = 0;
    if (ports) {
        for (; ports[n]; n++)
            ;
        jack_free(ports);
    }
    return n ? n : 1;
}

/* -----------------------------------------------------------------------
 * Init / quit
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_init(JackDawProject *project)
{
    jack_status_t status;
    jack_nframes_t sr, bs;
    char name[64];
    guint i;

    if (engine.active)
        return FALSE; /* already running */

    memset(&engine, 0, sizeof(engine));
    engine.project = project;

    /* Master output ports: honour a user-set count from a previous session
     * (Options -> Inputs/Outputs, persisted below), else a stereo master. The
     * master mix still only writes out_1/out_2; extra outs are silent until the
     * user routes to them in the patchbay. */
    {
        guint out_n = settings_get_uint32("jackAudioOutCount", 0);
        engine.audio_out_count = out_n ? CLAMP(out_n, 1u, (guint)JACKDAW_MAX_TRACKS) : 2u;
    }

    /* JackNoStartServer: connect only to the server the user already started
     * (e.g. from jack-graph's settings), never spawn or reconfigure one of our
     * own. If no server is running this fails cleanly and the user is told to
     * start jackd first — JackDAW is a plain client, not a server manager. */
    engine.client = jack_client_open("jackdaw", JackNoStartServer, &status);
    if (!engine.client) {
        jackdaw_error("Could not connect to the JACK server.\n"
                      "Start JACK first (e.g. from jack-graph), then launch JackDAW.");
        return TRUE;
    }

    /* Query JACK's actual sample rate and buffer size */
    sr = jack_get_sample_rate(engine.client);
    bs = jack_get_buffer_size(engine.client);
    engine.buf_size = bs;
    engine.sample_rate = sr;

    /* Pre-render the metronome click: a 1 kHz tone with a fast decay. */
    engine.click_len = (int)((double)sr * 0.025);
    engine.click_buf = g_malloc0((size_t)engine.click_len * sizeof(float));
    for (int n = 0; n < engine.click_len; n++) {
        double tt = (double)n / (double)sr;
        double env = exp(-tt * 120.0);
        engine.click_buf[n] = (float)(0.5 * sin(2.0 * M_PI * 1000.0 * tt) * env);
    }

    /* Allocate mix scratch buffers */
    engine.master_L = g_malloc0(bs * sizeof(float));
    engine.master_R = g_malloc0(bs * sizeof(float));
    engine.render_tap_L = g_malloc0(bs * sizeof(float));
    engine.render_tap_R = g_malloc0(bs * sizeof(float));
    /* Per-slot scratch for parallel track processing (all slots, occupied or
     * not, so a worker can always write its slot). */
    for (guint s = 0; s < JACKDAW_MAX_TRACKS; s++) {
        engine.slot_L[s] = g_malloc0(bs * sizeof(float));
        engine.slot_R[s] = g_malloc0(bs * sizeof(float));
    }

    /* Register callbacks */
    jack_set_thread_init_callback(engine.client, engine_thread_init_cb, NULL);
    jack_set_process_callback(engine.client, engine_process, NULL);
    jack_set_buffer_size_callback(engine.client, engine_buffer_size_cb, NULL);
    jack_set_xrun_callback(engine.client, engine_xrun_cb, NULL);
    jack_on_shutdown(engine.client, engine_shutdown_cb, NULL);
    jack_set_port_registration_callback(engine.client, engine_port_reg_cb, NULL);
    jack_set_port_connect_callback(engine.client, engine_port_connect_cb, NULL);

    /* Register audio output ports: out_1 .. out_N. The array is sized to the
     * hard track ceiling (not the live count) so growing/shrinking the pool at
     * runtime never reallocates memory the RT callback is reading — see
     * jackdaw_engine_set_audio_out_count. */
    engine.audio_out = g_new0(jack_port_t *, JACKDAW_MAX_TRACKS);
    for (i = 0; i < engine.audio_out_count; i++) {
        g_snprintf(name, sizeof(name), "out_%u", i + 1);
        engine.audio_out[i] =
            jack_port_register(engine.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!engine.audio_out[i])
            goto fail;
    }

    /* Dedicated metronome output (mono). Routed by the user (e.g. to a
     * performer's headphone bus) via the patchbay; not auto-connected. */
    engine.metro_out = jack_port_register(engine.client, "metronome", JACK_DEFAULT_AUDIO_TYPE,
                                          JackPortIsOutput, 0);
    if (!engine.metro_out)
        goto fail;

    /* Capture-port pool: one mono input ("in_N") per possible track, sized to
     * the number of physical capture channels the server exposes (clamped) so
     * every hardware input can be routed to a track. Right ports are registered
     * lazily by set_track_stereo. A track at slot i reads in_(i+1). */
    {
        guint in_n = settings_get_uint32("jackAudioInCount", 0);
        engine.audio_in_count =
            in_n ? CLAMP(in_n, 1u, (guint)JACKDAW_MAX_TRACKS)
                 : CLAMP(count_physical_ports(engine.client, JACK_DEFAULT_AUDIO_TYPE,
                                              JackPortIsOutput),
                         1, JACKDAW_MAX_TRACKS);
    }
    /* Sized to the track ceiling (see the output-port note above) so the
     * runtime count setter only (un)registers ports, never reallocates. */
    engine.audio_in = g_new0(jack_port_t *, JACKDAW_MAX_TRACKS);
    engine.audio_in_r = g_new0(jack_port_t *, JACKDAW_MAX_TRACKS);
    for (i = 0; i < engine.audio_in_count; i++) {
        g_snprintf(name, sizeof(name), "in_%u", i + 1);
        engine.audio_in[i] =
            jack_port_register(engine.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!engine.audio_in[i])
            goto fail;
    }

    /* MIDI capture/playback ports are registered on demand (per instrument-track
     * slot). control_in is a single dedicated control-surface input. */
    engine.midi_in = g_new0(jack_port_t *, JACKDAW_MAX_TRACKS);
    engine.midi_out = g_new0(jack_port_t *, JACKDAW_MAX_TRACKS);
    engine.control_in =
        jack_port_register(engine.client, "control_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

    /* Preview-note ring (main thread -> RT), for piano-roll auditioning. */
    eng_preview_rb = rt_ringbuffer_create(256 * sizeof(EngPrevMsg));
    if (eng_preview_rb)
        rt_ringbuffer_mlock(eng_preview_rb);

    /* Control-surface ring (RT -> main), drained by jackdaw_engine_control_poll. */
    eng_control_rb = rt_ringbuffer_create(1024 * sizeof(JackDawCtlEvent));
    if (eng_control_rb)
        rt_ringbuffer_mlock(eng_control_rb);

    /* Activate — after this the process callback can be called at any time */
    if (jack_activate(engine.client) != 0) {
        jackdaw_error("jackdaw: jack_activate() failed");
        goto fail;
    }

    /* Auto-connect out_N → physical audio playback ports by matching index, so
     * out_1→playback_1, out_2→playback_2, ... Don't rely on JACK's own
     * auto-connect (it's off unless the client requests it and many setups
     * disable it), so wire it here. EEXIST is fine. */
    {
        const char **phys_audio_in = jack_get_ports(engine.client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                                                    JackPortIsInput | JackPortIsPhysical);
        if (phys_audio_in) {
            guint pi;
            for (pi = 0; pi < engine.audio_out_count && phys_audio_in[pi]; pi++) {
                const char *src = jack_port_name(engine.audio_out[pi]);
                int r = jack_connect(engine.client, src, phys_audio_in[pi]);
                (void)r; /* EEXIST is fine */
            }
            /* Also monitor the dedicated metronome click through the same
             * physical outputs (mono -> both), so it is audible by default
             * while staying out of the DAW's main mix (never summed into the
             * master, meters or recordings). The user can rewire it to a
             * separate headphone bus in the patchbay. EEXIST is fine. */
            if (engine.metro_out) {
                const char *msrc = jack_port_name(engine.metro_out);
                for (guint mi = 0; mi < 2 && phys_audio_in[mi]; mi++)
                    (void)jack_connect(engine.client, msrc, phys_audio_in[mi]);
            }
            jack_free(phys_audio_in);
        }
    }

    engine.active = TRUE;

    /* Start the playback feeder thread (fills per-track play ringbuffers) and
     * the recorder thread (drains capture ringbuffers to disk). */
    feeder_start();
    recorder_start();

    /* Spawn the RT-priority worker pool for parallel track processing. Count
     * defaults to (CPU cores − 1) so the JACK thread itself is the Nth worker;
     * override with JACKDAW_RT_THREADS (0 = serial, for A/B comparison). Workers
     * are created via jack_client_create_thread so they inherit JACK's real-time
     * scheduling — a plain pthread would be preempted and make xruns worse. */
    {
        int want = (int)g_get_num_processors() - 1;
        const char *env = g_getenv("JACKDAW_RT_THREADS");
        if (env)
            want = atoi(env);
        if (want < 0)
            want = 0;
        if (want > RT_MAX_WORKERS)
            want = RT_MAX_WORKERS;

        if (want > 0) {
            int prio = jack_client_real_time_priority(engine.client);
            int rt = (prio > 0) ? 1 : 0;
            sem_init(&g_rt_sem_go, 0, 0);
            sem_init(&g_rt_sem_done, 0, 0);
            g_atomic_int_set(&g_rt_workers_quit, 0);
            int spawned = 0;
            for (int w = 0; w < want; w++) {
                if (jack_client_create_thread(engine.client, &g_rt_worker[w], prio, rt,
                                              rt_worker_main, NULL) != 0)
                    break;
                spawned++;
            }
            g_rt_nworkers = spawned;
            if (spawned == 0) {
                sem_destroy(&g_rt_sem_go);
                sem_destroy(&g_rt_sem_done);
            }
        }
        g_message("jackdaw: %d RT worker thread(s) for parallel track processing (%d cores)",
                  g_rt_nworkers, (int)g_get_num_processors());
    }

    /* Diagnostics: only when JACKDAW_DIAG is set. */
    g_diag_on = (g_getenv("JACKDAW_DIAG") != NULL);
    if (g_diag_on) {
        g_diag_period_us = sr ? (gint64)bs * 1000000 / sr : 0;
        g_atomic_int_set(&g_diag_quit, 0);
        g_diag_thread = g_thread_new("jackdaw-diag", diag_thread_func, NULL);
        g_message("[diag] enabled: buf=%u period=%ldus — watching xruns / callback time", bs,
                  (long)g_diag_period_us);
    }
    return FALSE; /* success */

fail:
    jack_client_close(engine.client);
    engine.client = NULL;
    g_free(engine.master_L);
    engine.master_L = NULL;
    g_free(engine.master_R);
    engine.master_R = NULL;
    for (guint s = 0; s < JACKDAW_MAX_TRACKS; s++) {
        g_free(engine.slot_L[s]);
        engine.slot_L[s] = NULL;
        g_free(engine.slot_R[s]);
        engine.slot_R[s] = NULL;
    }
    g_free(engine.render_tap_L);
    engine.render_tap_L = NULL;
    g_free(engine.render_tap_R);
    engine.render_tap_R = NULL;
    if (engine.render_rb_L) {
        rt_ringbuffer_free(engine.render_rb_L);
        engine.render_rb_L = NULL;
    }
    if (engine.render_rb_R) {
        rt_ringbuffer_free(engine.render_rb_R);
        engine.render_rb_R = NULL;
    }
    g_free(engine.click_buf);
    engine.click_buf = NULL;
    engine.click_len = 0;
    g_free(engine.audio_out);
    engine.audio_out = NULL;
    g_free(engine.audio_in);
    engine.audio_in = NULL;
    g_free(engine.audio_in_r);
    engine.audio_in_r = NULL;
    engine.audio_in_count = 0;
    g_free(engine.midi_in);
    engine.midi_in = NULL;
    g_free(engine.midi_out);
    engine.midi_out = NULL;
    engine.control_in = NULL;
    g_clear_pointer(&engine.control_src_port, g_free);
    if (eng_preview_rb) {
        rt_ringbuffer_free(eng_preview_rb);
        eng_preview_rb = NULL;
    }
    if (eng_control_rb) {
        rt_ringbuffer_free(eng_control_rb);
        eng_control_rb = NULL;
    }
    return TRUE;
}

void jackdaw_engine_quit(void)
{
    if (!engine.active || !engine.client)
        return;

    /* Stop the feeder and recorder before tearing down the client so neither
     * touches a closing JACK client or freed ringbuffers. */
    feeder_stop();
    recorder_stop();

    /* Stop the RT worker pool: signal quit, wake every worker, join. The workers
     * exit their loop cleanly, so a plain join (jack_native_thread_t is a
     * pthread here) is enough. Do this before jack_deactivate — the client that
     * created the threads is still open. */
    if (g_rt_nworkers > 0) {
        g_atomic_int_set(&g_rt_workers_quit, 1);
        for (int w = 0; w < g_rt_nworkers; w++)
            sem_post(&g_rt_sem_go);
        for (int w = 0; w < g_rt_nworkers; w++)
            pthread_join(g_rt_worker[w], NULL);
        sem_destroy(&g_rt_sem_go);
        sem_destroy(&g_rt_sem_done);
        g_rt_nworkers = 0;
    }

    if (g_diag_thread) {
        g_atomic_int_set(&g_diag_quit, 1);
        g_thread_join(g_diag_thread);
        g_diag_thread = NULL;
    }

    jack_deactivate(engine.client);
    jack_client_close(engine.client);
    engine.client = NULL;
    engine.active = FALSE;

    g_free(engine.master_L);
    engine.master_L = NULL;
    g_free(engine.master_R);
    engine.master_R = NULL;
    for (guint s = 0; s < JACKDAW_MAX_TRACKS; s++) {
        g_free(engine.slot_L[s]);
        engine.slot_L[s] = NULL;
        g_free(engine.slot_R[s]);
        engine.slot_R[s] = NULL;
    }
    g_free(engine.render_tap_L);
    engine.render_tap_L = NULL;
    g_free(engine.render_tap_R);
    engine.render_tap_R = NULL;
    if (engine.render_rb_L) {
        rt_ringbuffer_free(engine.render_rb_L);
        engine.render_rb_L = NULL;
    }
    if (engine.render_rb_R) {
        rt_ringbuffer_free(engine.render_rb_R);
        engine.render_rb_R = NULL;
    }
    g_free(engine.click_buf);
    engine.click_buf = NULL;
    engine.click_len = 0;
    g_free(engine.audio_out);
    engine.audio_out = NULL;
    g_free(engine.audio_in);
    engine.audio_in = NULL;
    g_free(engine.audio_in_r);
    engine.audio_in_r = NULL;
    engine.audio_in_count = 0;
    g_free(engine.midi_in);
    engine.midi_in = NULL;
    g_free(engine.midi_out);
    engine.midi_out = NULL;
    engine.control_in = NULL; /* unregistered by jack_client_close above */
    engine.metro_out = NULL;  /* unregistered by jack_client_close above */
    g_clear_pointer(&engine.control_src_port, g_free);
    if (eng_preview_rb) {
        rt_ringbuffer_free(eng_preview_rb);
        eng_preview_rb = NULL;
    }
    if (eng_control_rb) {
        rt_ringbuffer_free(eng_control_rb);
        eng_control_rb = NULL;
    }
}

/* -----------------------------------------------------------------------
 * State queries
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_is_running(void)
{
    return engine.active;
}

gboolean jackdaw_engine_is_recording(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_RECORDING) != 0;
}

gboolean jackdaw_engine_is_playing(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING) != 0;
}

/* -----------------------------------------------------------------------
 * Track management
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_add_track(JackDawTrack *track)
{
    guint i;

    g_return_val_if_fail(JACKDAW_IS_TRACK(track), TRUE);

    /* Find a free slot */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (!engine.slots[i])
            break;
    }
    if (i == JACKDAW_MAX_TRACKS) {
        jackdaw_error("jackdaw: maximum track count reached");
        return TRUE;
    }

    /* Allocate the playback ringbuffers (2 seconds at the detected sample rate);
     * the feeder writes them, the RT callback drains them. mlock keeps their
     * pages resident. Sized by sample rate, not buffer size, so a buffer-size
     * change never needs to touch them. */
    {
        jack_nframes_t sr = engine.client ? jack_get_sample_rate(engine.client) : 48000;
        size_t rb_bytes = (size_t)(2 * sr) * sizeof(float);
        track->play_buf_L = rt_ringbuffer_create(rb_bytes);
        track->play_buf_R = rt_ringbuffer_create(rb_bytes);
        if (!track->play_buf_L || !track->play_buf_R) {
            jackdaw_error("jackdaw: ringbuffer allocation failed");
            if (track->play_buf_L)
                rt_ringbuffer_free(track->play_buf_L);
            if (track->play_buf_R)
                rt_ringbuffer_free(track->play_buf_R);
            track->play_buf_L = track->play_buf_R = NULL;
            return TRUE;
        }
        rt_ringbuffer_mlock(track->play_buf_L);
        rt_ringbuffer_mlock(track->play_buf_R);

        /* Capture ringbuffers (same 2 s size): RT callback writes, recorder
         * thread drains. Recording write-through to disk keeps user audio safe
         * even if the ring backs up. A failed allocation just leaves them NULL —
         * the capture path guards on them, so the track simply cannot record. */
        track->rec_buf_L = rt_ringbuffer_create(rb_bytes);
        track->rec_buf_R = rt_ringbuffer_create(rb_bytes);
        if (track->rec_buf_L)
            rt_ringbuffer_mlock(track->rec_buf_L);
        if (track->rec_buf_R)
            rt_ringbuffer_mlock(track->rec_buf_R);
    }

    /* An audio track takes capture port in_(slot+1) as its input (if the pool
     * covers this slot). Instrument tracks take no audio input. The
     * playback/record ringbuffers are added with clips/recording (later phases).
     * Publish the slot last so the RT callback only sees a fully wired track. */
    if (jackdaw_track_is_instrument(track)) {
        track->audio_in_idx = -1;
        /* Register this instrument track's own MIDI capture + playback ports
         * (midi_in_N / midi_out_N) and a MIDI capture ring for recording. */
        track->midi_in_idx = (gint)i;
        track->midi_rec_buf = rt_ringbuffer_create(TRACK_MIDI_RINGBUF_BYTES);
        if (track->midi_rec_buf)
            rt_ringbuffer_mlock(track->midi_rec_buf);
        if (engine.client && engine.midi_in && !engine.midi_in[i]) {
            char mname[64];
            g_snprintf(mname, sizeof(mname), "midi_in_%u", i + 1);
            engine.midi_in[i] = jack_port_register(engine.client, mname, JACK_DEFAULT_MIDI_TYPE,
                                                   JackPortIsInput, 0);
        }
        if (engine.client && engine.midi_out && !engine.midi_out[i]) {
            char mname[64];
            g_snprintf(mname, sizeof(mname), "midi_out_%u", i + 1);
            engine.midi_out[i] = jack_port_register(engine.client, mname, JACK_DEFAULT_MIDI_TYPE,
                                                    JackPortIsOutput, 0);
            /* Auto-connect this output to the first physical MIDI playback port
             * so recorded/scheduled notes reach a synth by default (EEXIST is
             * fine; the user can rewire in the patchbay). */
            if (engine.midi_out[i] && engine.active) {
                const char **phys = jack_get_ports(engine.client, NULL, JACK_DEFAULT_MIDI_TYPE,
                                                   JackPortIsInput | JackPortIsPhysical);
                if (phys && phys[0])
                    (void)jack_connect(engine.client, jack_port_name(engine.midi_out[i]), phys[0]);
                if (phys)
                    jack_free((void *)phys);
            }
        }
    } else {
        track->audio_in_idx = (i < engine.audio_in_count) ? (gint)i : -1;
        track->midi_in_idx = -1;
    }
    track->slot = i;
    engine.slots[i] = track; /* RT callback can see this now */

    return FALSE;
}

void jackdaw_engine_remove_track(JackDawTrack *track)
{
    guint i;
    g_return_if_fail(JACKDAW_IS_TRACK(track));
    i = track->slot;
    if (i >= JACKDAW_MAX_TRACKS || engine.slots[i] != track)
        return;

    engine.slots[i] = NULL; /* RT callback stops using this slot */
    /* ...from the next cycle on. One already running loaded the slot and may
     * still be reading this track's ports, so wait it out before any of them
     * is unregistered below. */
    eng_wait_rt_past();

    /* Tear down any jackdaw-made input connections and the lazily-registered
     * right capture port before releasing the slot. */
    if (engine.active && engine.client && track->audio_in_idx >= 0 &&
        (guint)track->audio_in_idx < engine.audio_in_count) {
        guint ai = (guint)track->audio_in_idx;
        if (track->audio_src_port && engine.audio_in[ai])
            jack_disconnect(engine.client, track->audio_src_port,
                            jack_port_name(engine.audio_in[ai]));
        if (track->audio_src_port_r && engine.audio_in_r[ai])
            jack_disconnect(engine.client, track->audio_src_port_r,
                            jack_port_name(engine.audio_in_r[ai]));
        if (engine.audio_in_r[ai]) {
            /* The audio input is shared by index: another track's cycle may be
             * reading it, so hide it and wait before unregistering. */
            jack_port_t *pr = engine.audio_in_r[ai];
            g_atomic_pointer_set(&engine.audio_in_r[ai], NULL);
            eng_wait_rt_past();
            jack_port_unregister(engine.client, pr);
        }
    }
    /* Tear down the instrument track's MIDI capture/playback ports + connection. */
    if (engine.active && engine.client && i < JACKDAW_MAX_TRACKS) {
        if (engine.midi_in && engine.midi_in[i]) {
            if (track->midi_src_port)
                jack_disconnect(engine.client, track->midi_src_port,
                                jack_port_name(engine.midi_in[i]));
            jack_port_unregister(engine.client, engine.midi_in[i]);
            engine.midi_in[i] = NULL;
        }
        if (engine.midi_out && engine.midi_out[i]) {
            jack_port_unregister(engine.client, engine.midi_out[i]);
            engine.midi_out[i] = NULL;
        }
    }
    g_clear_pointer(&track->audio_src_port, g_free);
    g_clear_pointer(&track->audio_src_port_r, g_free);
    g_clear_pointer(&track->midi_src_port, g_free);
    track->stereo_input = FALSE;

    track->audio_in_idx = -1;
    track->midi_in_idx = -1;
    track->slot = G_MAXUINT;
}

/* -----------------------------------------------------------------------
 * Input routing
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_set_audio_source_l(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client)
        return TRUE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count || !engine.audio_in[(guint)ai])
        return TRUE;

    const char *dst = jack_port_name(engine.audio_in[(guint)ai]);

    /* Disconnect the current source, if any, first. */
    if (t->audio_src_port) {
        jack_disconnect(engine.client, t->audio_src_port, dst);
        g_clear_pointer(&t->audio_src_port, g_free);
    }

    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST)
            return TRUE;
        t->audio_src_port = g_strdup(port_name);
    }
    g_signal_emit_by_name(t, "routing-changed");
    return FALSE;
}

gboolean jackdaw_engine_set_audio_source_r(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client)
        return TRUE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count || !engine.audio_in_r[(guint)ai])
        return TRUE;

    const char *dst = jack_port_name(engine.audio_in_r[(guint)ai]);

    if (t->audio_src_port_r) {
        jack_disconnect(engine.client, t->audio_src_port_r, dst);
        g_clear_pointer(&t->audio_src_port_r, g_free);
    }

    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST)
            return TRUE;
        t->audio_src_port_r = g_strdup(port_name);
    }
    g_signal_emit_by_name(t, "routing-changed");
    return FALSE;
}

gboolean jackdaw_engine_set_track_stereo(JackDawTrack *t, gboolean stereo)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    t->stereo_input = stereo;

    if (!engine.active || !engine.client)
        return FALSE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count)
        return FALSE;

    if (stereo) {
        if (!engine.audio_in_r[(guint)ai]) {
            char name[64];
            g_snprintf(name, sizeof(name), "in_%uR", (guint)ai + 1);
            jack_port_t *pr = jack_port_register(engine.client, name, JACK_DEFAULT_AUDIO_TYPE,
                                                 JackPortIsInput, 0);
            if (!pr)
                return TRUE;
            g_atomic_pointer_set(&engine.audio_in_r[(guint)ai], pr);
        }
    } else {
        /* Clear the right source (so RT stops reading it), then drop the port. */
        if (t->audio_src_port_r) {
            if (engine.audio_in_r[(guint)ai])
                jack_disconnect(engine.client, t->audio_src_port_r,
                                jack_port_name(engine.audio_in_r[(guint)ai]));
            g_clear_pointer(&t->audio_src_port_r, g_free);
        }
        if (engine.audio_in_r[(guint)ai]) {
            jack_port_t *pr = engine.audio_in_r[(guint)ai];
            g_atomic_pointer_set(&engine.audio_in_r[(guint)ai], NULL);
            eng_wait_rt_past();
            jack_port_unregister(engine.client, pr);
        }
    }
    return FALSE;
}

gboolean jackdaw_engine_set_midi_source(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client)
        return TRUE;

    gint mi = t->midi_in_idx;
    if (mi < 0 || mi >= JACKDAW_MAX_TRACKS || !engine.midi_in || !engine.midi_in[mi])
        return TRUE;

    const char *dst = jack_port_name(engine.midi_in[mi]);
    if (t->midi_src_port) {
        jack_disconnect(engine.client, t->midi_src_port, dst);
        g_clear_pointer(&t->midi_src_port, g_free);
    }
    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST)
            return TRUE;
        t->midi_src_port = g_strdup(port_name);
    }
    g_signal_emit_by_name(t, "routing-changed");
    return FALSE;
}

const char **jackdaw_engine_list_capture_ports(void)
{
    if (!engine.active || !engine.client)
        return NULL;
    return jack_get_ports(engine.client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                          JackPortIsOutput | JackPortIsPhysical);
}

const char **jackdaw_engine_list_midi_ports(void)
{
    if (!engine.active || !engine.client)
        return NULL;
    return jack_get_ports(engine.client, NULL, JACK_DEFAULT_MIDI_TYPE,
                          JackPortIsOutput | JackPortIsPhysical);
}

void jackdaw_engine_free_ports(const char **ports)
{
    if (ports)
        jack_free((void *)ports);
}

/* ---- Control-surface input (main thread only) ---- */

gboolean jackdaw_engine_control_poll(JackDawCtlEvent *out)
{
    if (!eng_control_rb || !out)
        return FALSE;
    if (rt_ringbuffer_read_space(eng_control_rb) < sizeof(JackDawCtlEvent))
        return FALSE;
    rt_ringbuffer_read(eng_control_rb, (char *)out, sizeof(JackDawCtlEvent));
    return TRUE;
}

gboolean jackdaw_engine_set_control_source(const gchar *port_name)
{
    if (!engine.active || !engine.client || !engine.control_in)
        return TRUE;
    const char *dst = jack_port_name(engine.control_in);

    if (engine.control_src_port) {
        jack_disconnect(engine.client, engine.control_src_port, dst);
        g_clear_pointer(&engine.control_src_port, g_free);
    }
    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST)
            return TRUE;
        engine.control_src_port = g_strdup(port_name);
    }
    return FALSE;
}

const gchar *jackdaw_engine_get_control_source(void)
{
    return engine.control_src_port;
}

/* ---- Audio port-count management (main thread only) ----
 * Grow or shrink the pool of JACK capture (in_N) and master output (out_N)
 * ports. The backing arrays are pre-allocated at the track ceiling, so a count
 * change never reallocates memory the RT process callback is reading; we only
 * (un)register ports and publish the new count. Ordering against the RT thread:
 *   grow   - fill the new slots, then raise the count last (release store), so
 *            the callback reads a new slot only after it is fully wired;
 *   shrink - lower the count first, then unregister the now-idle ports, so the
 *            callback stops touching a slot before its port disappears.
 * Persisted immediately so the pool size survives a relaunch. Return FALSE on
 * success, TRUE on failure. */
guint jackdaw_engine_get_audio_in_count(void)
{
    return engine.audio_in_count;
}

guint jackdaw_engine_get_audio_out_count(void)
{
    return engine.audio_out_count;
}

gboolean jackdaw_engine_set_audio_in_count(guint n)
{
    guint i, old;
    char name[64];

    n = CLAMP(n, 1u, (guint)JACKDAW_MAX_TRACKS);
    if (!engine.active || !engine.client) {
        engine.audio_in_count = n;
        settings_set_uint32("jackAudioInCount", n);
        settings_save();
        return FALSE;
    }
    old = engine.audio_in_count;
    if (n == old)
        return FALSE;

    if (n > old) {
        for (i = old; i < n; i++) {
            engine.audio_in_r[i] = NULL; /* right port stays lazy (set_track_stereo) */
            g_snprintf(name, sizeof(name), "in_%u", i + 1);
            engine.audio_in[i] = jack_port_register(engine.client, name, JACK_DEFAULT_AUDIO_TYPE,
                                                    JackPortIsInput, 0);
            if (!engine.audio_in[i])
                return TRUE;
        }
        g_atomic_int_set((gint *)&engine.audio_in_count, (gint)n);
    } else {
        g_atomic_int_set((gint *)&engine.audio_in_count, (gint)n);
        eng_wait_rt_past();
        for (i = n; i < old; i++) {
            if (engine.audio_in[i]) {
                jack_port_unregister(engine.client, engine.audio_in[i]);
                engine.audio_in[i] = NULL;
            }
            if (engine.audio_in_r[i]) {
                jack_port_unregister(engine.client, engine.audio_in_r[i]);
                engine.audio_in_r[i] = NULL;
            }
        }
    }
    settings_set_uint32("jackAudioInCount", n);
    settings_save();
    return FALSE;
}

gboolean jackdaw_engine_set_audio_out_count(guint n)
{
    guint i, old;
    char name[64];

    n = CLAMP(n, 1u, (guint)JACKDAW_MAX_TRACKS);
    if (!engine.active || !engine.client) {
        engine.audio_out_count = n;
        settings_set_uint32("jackAudioOutCount", n);
        settings_save();
        return FALSE;
    }
    old = engine.audio_out_count;
    if (n == old)
        return FALSE;

    if (n > old) {
        for (i = old; i < n; i++) {
            g_snprintf(name, sizeof(name), "out_%u", i + 1);
            engine.audio_out[i] = jack_port_register(engine.client, name, JACK_DEFAULT_AUDIO_TYPE,
                                                     JackPortIsOutput, 0);
            if (!engine.audio_out[i])
                return TRUE;
        }
        g_atomic_int_set((gint *)&engine.audio_out_count, (gint)n);
    } else {
        g_atomic_int_set((gint *)&engine.audio_out_count, (gint)n);
        eng_wait_rt_past();
        for (i = n; i < old; i++) {
            if (engine.audio_out[i]) {
                jack_port_unregister(engine.client, engine.audio_out[i]);
                engine.audio_out[i] = NULL;
            }
        }
    }
    settings_set_uint32("jackAudioOutCount", n);
    settings_save();
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Transport
 * ----------------------------------------------------------------------- */

/* Open a WAV capture slot for one armed audio track. start_frame is the timeline
 * position the take begins at; expected_frames caps the capture length (0 =
 * open-ended, set later at stop). The caller must have verified the track is an
 * armed audio track. Runs on the main thread (does file I/O) BEFORE ENGINE_RECORDING
 * is set, so the RT thread is not yet touching this slot. Returns TRUE on success. */
gchar *jackdaw_recordings_dir(void)
{
    /* Scratch recordings live under the Haiku settings hierarchy
     * (<settings>/JackDAW/recordings), resolved via find_directory rather than a
     * hidden dot-directory in $HOME. Created if missing. */
    char base[B_PATH_NAME_LENGTH];
    if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, base, sizeof(base)) != B_OK)
        g_strlcpy(base, "/boot/home/config/settings", sizeof(base));
    gchar *dir = g_build_filename(base, "JackDAW", "recordings", NULL);
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

static gboolean recorder_open_slot(guint i, JackDawTrack *t, off_t start_frame,
                                   off_t expected_frames)
{
    if (recorder_slots[i].sf)
        return FALSE; /* already open */

    gchar *rec_dir = jackdaw_recordings_dir();

    jack_nframes_t sr = engine.client ? jack_get_sample_rate(engine.client) : 48000;
    GDateTime *now = g_date_time_new_now_local();
    gchar *ts = g_date_time_format(now, "%Y%m%d_%H%M%S");
    gchar *fname = g_strdup_printf("track_%u_%s.wav", i + 1, ts);
    gchar *fpath = g_build_filename(rec_dir, fname, NULL);
    g_strlcpy(recorder_slots[i].path, fpath, sizeof(recorder_slots[i].path));
    g_free(fpath);
    g_free(fname);
    g_free(ts);
    g_date_time_unref(now);
    g_free(rec_dir);

    int channels = t->stereo_input ? 2 : 1;

    SF_INFO sfi = {0};
    sfi.samplerate = (int)sr;
    sfi.channels = channels;
    sfi.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;

    SNDFILE *sf = sf_open(recorder_slots[i].path, SFM_WRITE, &sfi);
    if (!sf) {
        g_warning("jackdaw: could not open recording file %s: %s", recorder_slots[i].path,
                  sf_strerror(NULL));
        return FALSE;
    }
    recorder_slots[i].sf = sf;
    recorder_slots[i].written = 0;
    recorder_slots[i].expected_frames = expected_frames;
    recorder_slots[i].channels = channels;
    recorder_slots[i].punch = 0;
    g_atomic_int_set(&recorder_slots[i].finalize_req, 0);

    t->rec_start_frame = start_frame;

    /* Reset the capture ringbuffers (RECORDING isn't set yet, so the RT thread
     * is not writing them) so the take starts clean. */
    if (t->rec_buf_L)
        rt_ringbuffer_reset(t->rec_buf_L);
    if (t->rec_buf_R)
        rt_ringbuffer_reset(t->rec_buf_R);

    /* Live-waveform peak buffer (one min/max pair per JACK period), reused
     * across takes; allocated once and freed at track finalize. */
    if (!t->rec_peak_buf)
        t->rec_peak_buf = g_new(gfloat, REC_PEAK_MAX_BUCKETS * 2);
    t->rec_peak_count = 0;
    t->rec_peak_block = engine.client ? (gint)jack_get_buffer_size(engine.client) : 1024;

    /* Capture latency of the input port, for alignment compensation. */
    t->rec_latency = 0;
    if ((guint)t->audio_in_idx < engine.audio_in_count && engine.audio_in[(guint)t->audio_in_idx]) {
        jack_latency_range_t lr = {0, 0};
        jack_port_get_latency_range(engine.audio_in[(guint)t->audio_in_idx], JackCaptureLatency,
                                    &lr);
        t->rec_latency = (off_t)lr.max;
    }
    return TRUE;
}

/* Arm every armed audio track for capture at the current play_pos. Main thread,
 * BEFORE ENGINE_RECORDING is set. Shared by immediate recording and count-in
 * recording (the slots wait, pre-opened, through the pre-roll). */
static void recorder_arm_all(void)
{
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t)
            continue;
        if (!(g_atomic_int_get(&t->state_flags) & TRACK_ARMED))
            continue;
        if (jackdaw_track_is_instrument(t)) /* audio only in this phase */
            continue;
        if (t->audio_in_idx < 0)
            continue;
        recorder_open_slot(i, t, (off_t)engine.play_pos, 0 /* open-ended */);
    }
}

/* Arm every armed instrument track for MIDI capture at the current play_pos:
 * anchor the take origin and reset the capture ring. Main thread, BEFORE
 * ENGINE_RECORDING is set (the RT thread is not yet writing the ring). Shared by
 * immediate recording and count-in recording. */
static void midi_arm_all(void)
{
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t || !jackdaw_track_is_instrument(t))
            continue;
        if (!(g_atomic_int_get(&t->state_flags) & TRACK_ARMED))
            continue;
        t->rec_start_frame = (off_t)engine.play_pos;
        if (t->midi_rec_buf)
            rt_ringbuffer_reset(t->midi_rec_buf);
    }
}

void jackdaw_engine_start_playback(void)
{
    /* Punch in/out: when the record mode is punch and a tab region lies at or
     * ahead of the playhead, pre-open capture slots for every armed audio track.
     * The RT path engages recording as the playhead crosses loop_start and stops
     * it at loop_end. Independent of the loop button — only the tab positions are
     * used, and playback never wraps. */
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    if (g_atomic_int_get(&engine.record_mode) == RECORD_MODE_PUNCH &&
        !g_atomic_int_get(&engine.punch_armed) && le > ls && engine.play_pos < le) {
        gboolean any = FALSE;
        for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            JackDawTrack *t = engine.slots[i];
            if (!t)
                continue;
            if (!(g_atomic_int_get(&t->state_flags) & TRACK_ARMED))
                continue;
            if (jackdaw_track_is_instrument(t))
                continue;
            if (t->audio_in_idx < 0)
                continue;
            if (!recorder_open_slot(i, t, ls, le - ls))
                continue;
            recorder_slots[i].punch = 1;
            recorder_slots[i].punch_tl_start = ls;
            recorder_slots[i].punch_tl_end = le;
            any = TRUE;
        }
        if (any)
            g_atomic_int_set(&engine.punch_armed, 1);
    }

    g_atomic_int_or(&engine.transport_flags, ENGINE_PLAYING);
}

void jackdaw_engine_stop_playback(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_PLAYING);

    /* Release any instrument-track notes left sounding by the snapshot so a stop
     * mid-note doesn't hang it (the RT flush emits the matching note-offs). */
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
        g_atomic_int_set(&eng_midi_flush[i], 1);

    /* Cancel a count-in pre-roll that never reached its hand-off. Finalize any
     * capture slots pre-opened for a record count-in (an unwritten take is
     * dropped by the recorder thread). */
    if (g_atomic_int_get(&engine.countin_active)) {
        gboolean was_rec = g_atomic_int_get(&engine.countin_pending_rec);
        g_atomic_int_set(&engine.countin_active, 0);
        g_atomic_int_set(&engine.countin_pending_rec, 0);
        if (was_rec) {
            for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
                if (recorder_slots[i].sf) {
                    recorder_slots[i].expected_frames = 0;
                    g_atomic_int_set(&recorder_slots[i].finalize_req, 1);
                }
        }
    }

    /* Cancel any pending/in-progress punch: stop capture and let the recorder
     * thread finalize whatever was recorded (an empty take is discarded). */
    if (g_atomic_int_get(&engine.punch_armed)) {
        g_atomic_int_set(&engine.punch_armed, 0);
        g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);
        for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
            if (recorder_slots[i].sf && recorder_slots[i].punch)
                g_atomic_int_set(&recorder_slots[i].finalize_req, 1);
    }
}

void jackdaw_engine_start_recording(void)
{
    /* Open capture slots for all armed audio tracks BEFORE setting RECORDING, so
     * the RT thread never opens a file. Then start rolling — the RT callback
     * begins filling the capture ringbuffers immediately. */
    recorder_arm_all();
    midi_arm_all();
    g_atomic_int_or(&engine.transport_flags, ENGINE_RECORDING | ENGINE_PLAYING);
}

void jackdaw_engine_stop_recording(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);

    /* A record count-in that hasn't engaged yet: clear it so the pre-roll won't
     * hand off into recording. Pre-opened slots are finalized (empty/dropped) by
     * the loop below. */
    g_atomic_int_set(&engine.countin_active, 0);
    g_atomic_int_set(&engine.countin_pending_rec, 0);
    g_atomic_int_set(&engine.punch_armed, 0);

    /* Capture play_pos NOW — the exact cut point for every recording track. Write
     * expected_frames before signalling finalize so the recorder thread sees the
     * cap (the atomic set provides the release barrier). */
    off_t cut = (off_t)engine.play_pos;
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (!recorder_slots[i].sf)
            continue;
        JackDawTrack *t = engine.slots[i];
        off_t exp = t ? (cut - t->rec_start_frame) : 0;
        recorder_slots[i].expected_frames = exp > 0 ? exp : 0;
        g_atomic_int_set(&recorder_slots[i].finalize_req, 1);
    }

    /* Release any instrument-track notes still held, and hand off MIDI capture to
     * the main thread (drains the ring into clip notes). RECORDING is now clear,
     * so the RT thread has stopped writing the MIDI ring; the SPSC ring is safe to
     * read concurrently with a trailing RT write, and finalize runs on the window
     * looper (this thread's next message). */
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
        g_atomic_int_set(&eng_midi_flush[i], 1);
    eng_midi_rec_cut = cut;
    engine_post_event(JACKDAW_ENGINE_EVENT_MIDI_TAKE_FINALIZED);
}

/* Drain finished takes into placed regions. MAIN thread only (creates AudioClips
 * and edits main-thread-owned region lists). Called in response to
 * JACKDAW_ENGINE_EVENT_TAKE_FINALIZED. */
void jackdaw_engine_finalize_takes(void)
{
    if (!recorder_done_q)
        return;
    RecordFinalize *rf;
    while ((rf = g_async_queue_try_pop(recorder_done_q)) != NULL) {
        GError *err = NULL;
        AudioClip *clip = audio_clip_new(rf->path, &err);
        if (clip && rf->punch) {
            /* Punch take: overwrite the existing audio in the tab region, then
             * drop the new clip in at the punch-in point (no latency shift — it
             * must line up exactly with the cleared range). */
            int sr = (int)jackdaw_engine_get_sample_rate();
            GPtrArray *regions = jackdaw_track_get_regions(rf->track);
            clip_region_list_delete_range(regions, rf->punch_tl_start, rf->punch_tl_end, sr);
            rf->track->clip_start = rf->punch_tl_start;
            jackdaw_track_place_clip(rf->track, clip, rf->punch_tl_start);
        } else if (clip) {
            /* Place the recording as a new region at the start point, shifted
             * earlier by the capture latency so it sits under the audio the
             * performer actually played against. */
            off_t tl = rf->track->rec_start_frame - rf->track->rec_latency;
            if (tl < 0)
                tl = 0;
            rf->track->clip_start = tl;
            jackdaw_track_place_clip(rf->track, clip, tl); /* consumes clip ref */
        } else {
            g_warning("jackdaw: could not load recording %s: %s", rf->path,
                      err ? err->message : "unknown");
            if (err)
                g_error_free(err);
        }
        /* Stop the live overlay now that the real clip is on the timeline. */
        rf->track->rec_peak_count = 0;
        g_object_unref(rf->track);
        g_free(rf);
    }
}

/* Convert each armed instrument track's captured MIDI into clip notes. MAIN
 * thread only (edits the main-thread-owned clip + republishes the RT snapshot).
 * Called in response to JACKDAW_ENGINE_EVENT_MIDI_TAKE_FINALIZED. */
void jackdaw_engine_finalize_midi_takes(void)
{
    double bpm = (engine.project && engine.project->bpm > 0.0) ? engine.project->bpm : 120.0;
    double fpb = (double)engine.sample_rate * 60.0 / bpm;
    double f_per_tick = (fpb > 0.0) ? fpb / (double)JACKDAW_PPQ : 1.0;
    off_t cut = eng_midi_rec_cut;

    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t || !jackdaw_track_is_instrument(t) || !t->midi_rec_buf)
            continue;
        if (rt_ringbuffer_read_space(t->midi_rec_buf) < sizeof(MidiRecEvent))
            continue;

        /* Notes are stored at ABSOLUTE ticks (tick 0 = timeline frame 0), so the
         * default full-clip region at tl_pos 0 plays them back exactly where they
         * were performed. on_frame holds the absolute capture frame. */
        gint64 on_frame[16][128];
        guint8 on_vel[16][128];
        for (int ch = 0; ch < 16; ch++)
            for (int p = 0; p < 128; p++)
                on_frame[ch][p] = -1;

        MidiClip *c = midi_clip_new(0);
        gint64 last_frame = t->rec_start_frame;

        MidiRecEvent r;
        while (rt_ringbuffer_read_space(t->midi_rec_buf) >= sizeof r) {
            rt_ringbuffer_read(t->midi_rec_buf, (char *)&r, sizeof r);
            if (r.frame > last_frame)
                last_frame = r.frame;
            int st = r.data[0] & 0xF0, ch = r.data[0] & 0x0F, p = r.data[1] & 0x7F;
            if (st == 0x90 && r.data[2] > 0) {
                on_frame[ch][p] = r.frame;
                on_vel[ch][p] = r.data[2];
            } else if (st == 0x80 || (st == 0x90 && r.data[2] == 0)) {
                if (on_frame[ch][p] < 0)
                    continue;
                gint64 dur = r.frame - on_frame[ch][p];
                if (dur < 0)
                    dur = 0;
                MidiNote n = {(guint32)((double)on_frame[ch][p] / f_per_tick),
                              (guint32)((double)dur / f_per_tick), (guint8)p, on_vel[ch][p],
                              (guint8)ch};
                if (n.length < 1)
                    n.length = 1;
                midi_clip_add_note(c, n);
                on_frame[ch][p] = -1;
            }
        }

        /* Close notes still held at the stop point (no note-off was captured). */
        off_t close_frame = (cut > last_frame) ? cut : last_frame;
        for (int ch = 0; ch < 16; ch++)
            for (int p = 0; p < 128; p++) {
                if (on_frame[ch][p] < 0)
                    continue;
                gint64 ef = (gint64)close_frame;
                if (ef < on_frame[ch][p])
                    ef = on_frame[ch][p];
                MidiNote n = {(guint32)((double)on_frame[ch][p] / f_per_tick),
                              (guint32)((double)(ef - on_frame[ch][p]) / f_per_tick), (guint8)p,
                              on_vel[ch][p], (guint8)ch};
                if (n.length < 1)
                    n.length = 1;
                midi_clip_add_note(c, n);
            }

        if (midi_clip_note_count(c) == 0) {
            midi_clip_free(c);
            continue;
        }

        /* Merge recorded notes into the track's single clip (absolute ticks). */
        MidiClip *dst = jackdaw_track_get_midi_clip(t);
        for (guint ni = 0; ni < midi_clip_note_count(c); ni++)
            midi_clip_add_note(dst, *midi_clip_note(c, ni));
        midi_clip_free(c);
        /* Re-seed a default region if every section was moved off this track, so
         * the freshly recorded notes are audible. */
        jackdaw_track_ensure_midi_region(t);
        jackdaw_track_commit_midi(t, fpb); /* publishes RT snapshot + redraws */
    }
}

#define ENG_REC_PREVIEW_MAX 16384
const JackDawRecNote *jackdaw_engine_rec_preview(JackDawTrack *t, guint *count)
{
    static MidiRecEvent ev[ENG_REC_PREVIEW_MAX];
    static JackDawRecNote notes[ENG_REC_PREVIEW_MAX];
    if (count)
        *count = 0;
    if (!t || !t->midi_rec_buf)
        return NULL;

    size_t avail = rt_ringbuffer_read_space(t->midi_rec_buf);
    guint ne = (guint)(avail / sizeof(MidiRecEvent));
    if (ne == 0)
        return NULL;
    if (ne > ENG_REC_PREVIEW_MAX)
        ne = ENG_REC_PREVIEW_MAX;
    size_t got = rt_ringbuffer_peek(t->midi_rec_buf, (char *)ev, (size_t)ne * sizeof(MidiRecEvent));
    ne = (guint)(got / sizeof(MidiRecEvent));

    off_t now = (off_t)engine.play_pos;
    gint on_idx[16][128];
    for (int ch = 0; ch < 16; ch++)
        for (int p = 0; p < 128; p++)
            on_idx[ch][p] = -1;

    guint nn = 0;
    for (guint e = 0; e < ne; e++) {
        int st = ev[e].data[0] & 0xF0, ch = ev[e].data[0] & 0x0F, p = ev[e].data[1] & 0x7F;
        gboolean is_on = (st == 0x90 && ev[e].data[2] > 0);
        gboolean is_off = (st == 0x80 || (st == 0x90 && ev[e].data[2] == 0));

        /* Any note-on or note-off for this pitch ends a note still open, so a
         * released OR re-triggered note stops extending to the playhead. */
        if ((is_on || is_off) && on_idx[ch][p] >= 0) {
            notes[on_idx[ch][p]].end_frame = (off_t)ev[e].frame;
            on_idx[ch][p] = -1;
        }
        if (is_on) {
            if (nn >= ENG_REC_PREVIEW_MAX)
                break;
            notes[nn].start_frame = (off_t)ev[e].frame;
            notes[nn].end_frame = now; /* held -> extend to playhead */
            notes[nn].pitch = (guint8)p;
            notes[nn].velocity = ev[e].data[2];
            notes[nn].channel = (guint8)ch;
            on_idx[ch][p] = (gint)nn;
            nn++;
        }
    }
    if (count)
        *count = nn;
    return nn ? notes : NULL;
}

void jackdaw_engine_preview_note(JackDawTrack *t, guint8 pitch, guint8 velocity, guint8 channel,
                                 gboolean on)
{
    if (!engine.active || !eng_preview_rb || !t)
        return;
    if (!jackdaw_track_is_instrument(t) || t->slot >= JACKDAW_MAX_TRACKS)
        return;
    EngPrevMsg msg;
    msg.slot = (gint32)t->slot;
    msg.data[0] = (guint8)((on ? 0x90 : 0x80) | (channel & 0x0F));
    msg.data[1] = pitch & 0x7F;
    msg.data[2] = on ? velocity : 0;
    if (rt_ringbuffer_write_space(eng_preview_rb) >= sizeof msg)
        rt_ringbuffer_write(eng_preview_rb, (const char *)&msg, sizeof msg);
}

/* Begin a count-in pre-roll, then start playback (record=FALSE) or recording
 * (record=TRUE) when it elapses. `beats` is the number of metronome clicks to
 * sound first. Returns FALSE (caller should start immediately) when no pre-roll
 * is possible: engine not running, beats==0, or a degenerate tempo. The project
 * playhead stays put during the pre-roll, so when recording the take begins
 * exactly at the cursor. */
gboolean jackdaw_engine_begin_countin(guint beats, gboolean record)
{
    if (!engine.active || beats == 0)
        return FALSE;
    if (g_atomic_int_get(&engine.countin_active))
        return TRUE; /* already counting */

    double bpm = (engine.project && engine.project->bpm > 0.0) ? engine.project->bpm : 120.0;
    double fpb = (double)engine.sample_rate * 60.0 / bpm;
    off_t len = (off_t)(fpb * (double)beats + 0.5);
    if (len <= 0)
        return FALSE;

    /* For a record count-in, pre-open the capture slots now so recording engages
     * instantly (no file I/O on the RT thread) when the pre-roll ends. */
    if (record) {
        recorder_arm_all();
        midi_arm_all();
    }

    engine.countin_pos = 0;
    engine.countin_len = len;
    g_atomic_int_set(&engine.countin_pending_rec, record ? 1 : 0);
    g_atomic_int_set(&engine.countin_active, 1); /* publish last */
    return TRUE;
}

gboolean jackdaw_engine_is_counting_in(void)
{
    return g_atomic_int_get(&engine.countin_active) != 0;
}

void jackdaw_engine_locate(off_t sample)
{
    guint i;
    /* Stop first: with ENGINE_PLAYING clear, neither the RT drain nor the feeder
     * touches the play ringbuffers, so resetting them here is race-free. */
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    engine.play_pos = sample;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t)
            continue;
        t->played_frames = sample;
        if (t->play_buf_L)
            rt_ringbuffer_reset(t->play_buf_L);
        if (t->play_buf_R)
            rt_ringbuffer_reset(t->play_buf_R);
        /* Tell the feeder to re-seek this slot to the new position. */
        feeder_slots[i].locate_frame = sample;
        g_atomic_int_set(&feeder_slots[i].locate_req, 1);
    }
}

/* -----------------------------------------------------------------------
 * Loop region / record mode
 * ----------------------------------------------------------------------- */

void jackdaw_engine_set_loop_range(off_t start, off_t end)
{
    if (end < start) {
        off_t tmp = start;
        start = end;
        end = tmp;
    }
    if (start < 0)
        start = 0;
    if (end < 0)
        end = 0;
    /* Seqlock write (see eng_loop_read): odd, the pair, even. The release
     * fence keeps the pair stores from overtaking the odd count; the final
     * increment is a full barrier, so the pair is visible before the even. */
    g_atomic_int_inc(&engine.loop_seq);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&engine.loop_start, start, __ATOMIC_RELAXED);
    __atomic_store_n(&engine.loop_end, end, __ATOMIC_RELAXED);
    g_atomic_int_inc(&engine.loop_seq);
}

void jackdaw_engine_get_loop_range(off_t *start, off_t *end)
{
    /* Called from window threads and the offline render worker, never from
     * RT, so it may wait out a writer mid-update. */
    EngLoopRange r;
    while (eng_loop_read(&r))
        g_thread_yield();
    if (start)
        *start = r.start;
    if (end)
        *end = r.end;
}

void jackdaw_engine_set_loop_enabled(gboolean on)
{
    g_atomic_int_set(&engine.loop_enabled, on ? 1 : 0);
}

gboolean jackdaw_engine_get_loop_enabled(void)
{
    return g_atomic_int_get(&engine.loop_enabled) != 0;
}

gboolean jackdaw_engine_has_loop_region(void)
{
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    return le > ls;
}

void jackdaw_engine_set_record_mode(int mode)
{
    g_atomic_int_set(&engine.record_mode,
                     mode == RECORD_MODE_PUNCH ? RECORD_MODE_PUNCH : RECORD_MODE_NORMAL);
}

int jackdaw_engine_get_record_mode(void)
{
    return g_atomic_int_get(&engine.record_mode);
}

/* -----------------------------------------------------------------------
 * Queries
 * ----------------------------------------------------------------------- */

jack_nframes_t jackdaw_engine_get_sample_rate(void)
{
    if (!engine.client)
        return 48000;
    return jack_get_sample_rate(engine.client);
}

jack_nframes_t jackdaw_engine_get_buffer_size(void)
{
    if (!engine.client)
        return 1024;
    return jack_get_buffer_size(engine.client);
}

off_t jackdaw_engine_get_play_pos(void)
{
    return engine.play_pos;
}

void jackdaw_engine_get_master_peaks(gfloat *out_L, gfloat *out_R)
{
    if (out_L) {
        *out_L = engine.master_peak_L;
        engine.master_peak_L = 0.0f;
    }
    if (out_R) {
        *out_R = engine.master_peak_R;
        engine.master_peak_R = 0.0f;
    }
}

guint jackdaw_engine_get_xrun_count(void)
{
    return (guint)g_atomic_int_get(&engine_xruns);
}

guint jackdaw_engine_get_cycle_count(void)
{
    return (guint)g_atomic_int_get(&engine_cycles);
}

gboolean jackdaw_engine_is_processing(void)
{
    return engine.active;
}

/* ======================================================================
 * Render / export primitives (P11)
 * ----------------------------------------------------------------------
 * Offline render drives these directly from a non-RT worker thread; the RT
 * callback is held off the plugins with render_suspend while it runs. The
 * realtime path taps the post-fader master into a ring drained by a writer.
 * libsamplerate is always present on Haiku (as in the feeder), so the Linux
 * HAVE_SAMPLERATE guards are dropped here. */

/* Scratch sizes for one reader chunk; matches the feeder's headroom rationale
 * so the resampler always has enough input frames staged. */
#define RR_RAW_FRAMES (4096 * 6)
#define RR_MAX_CHANNELS 8

/* A synchronous, render-only equivalent of one feeder slot: reads a contiguous
 * span of a track's timeline audio into caller buffers, resampling clip→render
 * SR as needed. It deliberately duplicates the feeder's clip-walk rather than
 * refactoring the live path, to avoid any risk of regressing realtime playback. */
struct EngTrackReader {
    int render_sr;
    ClipRegionSnapshot *snap; /* held ref; regions are stable while suspended */
    SNDFILE *sf;
    int open_region;
    int open_clip_sr, open_clip_ch;
    SRC_STATE *src_L, *src_R;
    float *raw;  /* RR_RAW_FRAMES * RR_MAX_CHANNELS */
    float *mono; /* RR_RAW_FRAMES */
};

static void eng_reader_close_file(EngTrackReader *r)
{
    if (r->sf) {
        sf_close(r->sf);
        r->sf = NULL;
    }
    if (r->src_L) {
        src_delete(r->src_L);
        r->src_L = NULL;
    }
    if (r->src_R) {
        src_delete(r->src_R);
        r->src_R = NULL;
    }
    r->open_region = -1;
}

EngTrackReader *engine_track_reader_new(JackDawTrack *t, int render_sr)
{
    EngTrackReader *r = g_new0(EngTrackReader, 1);
    r->render_sr = render_sr;
    r->open_region = -1;
    r->snap = jackdaw_track_ref_snapshot(t); /* may be NULL */
    r->raw = g_new(float, RR_RAW_FRAMES *RR_MAX_CHANNELS);
    r->mono = g_new(float, RR_RAW_FRAMES);
    return r;
}

void engine_track_reader_free(EngTrackReader *r)
{
    if (!r)
        return;
    eng_reader_close_file(r);
    if (r->snap)
        clip_region_snapshot_unref(r->snap);
    g_free(r->raw);
    g_free(r->mono);
    g_free(r);
}

/* Fill outL/outR (caller-owned, >= n frames) with the track's timeline audio in
 * [start, start+n), region gain applied, resampled to render_sr. Gaps/missing
 * files become silence. n must be <= RR_RAW_FRAMES. Returns FALSE (success). */
gboolean engine_track_reader_read(EngTrackReader *r, JackDawTrack *t, off_t start, jack_nframes_t n,
                                  float *outL, float *outR)
{
    (void)t;
    memset(outL, 0, n * sizeof(float));
    memset(outR, 0, n * sizeof(float));
    ClipRegionSnapshot *snap = r->snap;
    if (!snap || snap->n == 0)
        return FALSE;

    int sr = r->render_sr;
    jack_nframes_t done = 0;
    off_t pf = start;

    while (done < n) {
        jack_nframes_t want = n - done;

        /* Region covering pf, plus the nearest region start after pf. */
        ClipRegion *reg = NULL;
        int reg_idx = -1;
        off_t next_start = -1;
        for (int k = 0; k < snap->n; k++) {
            ClipRegion *cr = &snap->r[k];
            if (pf >= cr->tl_pos && pf < cr->tl_pos + cr->length) {
                reg = cr;
                reg_idx = k;
                break;
            }
            if (cr->tl_pos > pf && (next_start < 0 || cr->tl_pos < next_start))
                next_start = cr->tl_pos;
        }

        if (!reg) {
            /* Gap / before first / past end → leave silence, advance. */
            off_t sil = want;
            if (next_start >= 0) {
                off_t to_next = next_start - pf;
                if (to_next > 0 && to_next < (off_t)sil)
                    sil = to_next;
            }
            done += (jack_nframes_t)sil;
            pf += sil;
            if (next_start < 0)
                break; /* nothing more ahead */
            continue;
        }

        int clip_sr = reg->clip ? reg->clip->info.samplerate : sr;
        int clip_ch = reg->clip ? reg->clip->info.channels : 1;
        int eff_ch = clip_ch > RR_MAX_CHANNELS ? RR_MAX_CHANNELS : clip_ch;
        gboolean needs_src = (clip_sr != sr);
        off_t d = pf - reg->tl_pos;
        off_t reg_remain = reg->length - d;
        if (reg_remain <= 0) {
            pf = reg->tl_pos + reg->length;
            continue;
        }

        off_t chunk = want;
        if (chunk > reg_remain)
            chunk = (off_t)reg_remain;

        if (reg_idx != r->open_region || !r->sf) {
            eng_reader_close_file(r);
            SF_INFO sfi = {0};
            SNDFILE *sf = reg->clip ? sf_open(reg->clip->path, SFM_READ, &sfi) : NULL;
            if (!sf) {
                done += (jack_nframes_t)chunk;
                pf += chunk;
                continue;
            }
            off_t file_off =
                reg->file_in + ((clip_sr == sr) ? d : (off_t)((double)d * clip_sr / sr + 0.5));
            sf_seek(sf, file_off, SEEK_SET);
            r->sf = sf;
            r->open_clip_sr = clip_sr;
            r->open_clip_ch = clip_ch;
            r->open_region = reg_idx;
            if (needs_src) {
                int e = 0;
                r->src_L = src_new(SRC_SINC_FASTEST, 1, &e);
                if (eff_ch > 1)
                    r->src_R = src_new(SRC_SINC_FASTEST, 1, &e);
            }
        }

        gfloat gain = reg->gain;
        float *dstL = outL + done;
        float *dstR = outR + done;

        if (!needs_src) {
            sf_count_t got = sf_readf_float(r->sf, r->raw, (sf_count_t)chunk);
            if (got < 0)
                got = 0;
            if (eff_ch == 1) {
                for (sf_count_t f = 0; f < got; f++)
                    dstL[f] = dstR[f] = r->raw[f] * gain;
            } else {
                for (sf_count_t f = 0; f < got; f++) {
                    dstL[f] = r->raw[f * eff_ch] * gain;
                    dstR[f] = r->raw[f * eff_ch + 1] * gain;
                }
            }
            /* tail beyond `got` stays zero from the initial memset */
        } else if (r->src_L) {
            double ratio = (double)sr / (double)clip_sr;
            long want_l = (long)chunk;
            long in_need = (long)ceil((double)chunk / ratio) + 8;
            if (in_need > RR_RAW_FRAMES)
                in_need = RR_RAW_FRAMES;
            int eoi = (chunk == reg_remain);

            sf_count_t got = sf_readf_float(r->sf, r->raw, (sf_count_t)in_need);
            if (got < 0)
                got = 0;

            for (sf_count_t f = 0; f < got; f++)
                r->mono[f] = r->raw[f * eff_ch];
            SRC_DATA sd_L = {.data_in = r->mono,
                             .data_out = dstL,
                             .input_frames = (long)got,
                             .output_frames = want_l,
                             .src_ratio = ratio,
                             .end_of_input = eoi};
            src_process(r->src_L, &sd_L);
            long out_gen = sd_L.output_frames_gen;

            if (eff_ch > 1 && r->src_R) {
                for (sf_count_t f = 0; f < got; f++)
                    r->mono[f] = r->raw[f * eff_ch + 1];
                SRC_DATA sd_R = {.data_in = r->mono,
                                 .data_out = dstR,
                                 .input_frames = (long)got,
                                 .output_frames = want_l,
                                 .src_ratio = ratio,
                                 .end_of_input = eoi};
                src_process(r->src_R, &sd_R);
            } else if (out_gen > 0) {
                memcpy(dstR, dstL, (size_t)out_gen * sizeof(float));
            }
            for (long x = 0; x < out_gen; x++) {
                dstL[x] *= gain;
                dstR[x] *= gain;
            }

            long used = sd_L.input_frames_used;
            if (used < got)
                sf_seek(r->sf, -(sf_count_t)(got - used), SEEK_CUR);
        }
        /* else: SRC needed but instance missing — leave silence */

        done += (jack_nframes_t)chunk;
        pf += chunk;
    }
    return FALSE;
}

/* Render-only MIDI gather for an instrument track: emit just the sequenced
 * events from the published snapshot that fall in [blk_start, blk_start+n). */
int eng_gather_render_midi(JackDawTrack *t, off_t blk_start, jack_nframes_t nframes,
                           PhMidiEvent *mev, int cap)
{
    int nev = 0;
    MidiEventSnapshot *ms = g_atomic_pointer_get(&t->rt_midi);
    if (!ms || !ms->n)
        return 0;

    off_t end = blk_start + nframes;
    guint lo = 0, hi = ms->n; /* lower_bound(blk_start) */
    while (lo < hi) {
        guint mid = (lo + hi) / 2;
        if (ms->ev[mid].frame < blk_start)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (guint e = lo; e < ms->n && ms->ev[e].frame < end && nev < cap; e++) {
        MidiSnapEvent *se = &ms->ev[e];
        mev[nev].time = (guint32)(se->frame - blk_start);
        mev[nev].size = 3;
        mev[nev].data[0] = se->s;
        mev[nev].data[1] = se->d1;
        mev[nev].data[2] = se->d2;
        nev++;
    }
    return nev;
}

void jackdaw_engine_render_suspend(gboolean on)
{
    g_atomic_int_set(&engine.render_suspend, on ? 1 : 0);
}

/* Suspend the live audio graph while the main thread instantiates or frees
 * plugins (project load). Same effect as render_suspend: the RT callback
 * outputs silence and runs no plugins, so heavy non-RT work (VST3 module load,
 * setupProcessing, buffer allocation) can't stall the audio thread into an
 * xrun. There is nothing to play during a load anyway. */
void jackdaw_engine_set_suspended(gboolean on)
{
    g_atomic_int_set(&engine.render_suspend, on ? 1 : 0);
}

void jackdaw_engine_render_tap_start(off_t end_frame)
{
    size_t bytes = (size_t)engine.buf_size * 64 * sizeof(float);
    if (bytes < 65536)
        bytes = 65536;
    if (!engine.render_rb_L)
        engine.render_rb_L = rt_ringbuffer_create(bytes);
    if (!engine.render_rb_R)
        engine.render_rb_R = rt_ringbuffer_create(bytes);
    if (engine.render_rb_L)
        rt_ringbuffer_reset(engine.render_rb_L);
    if (engine.render_rb_R)
        rt_ringbuffer_reset(engine.render_rb_R);
    engine.render_end = end_frame;
    g_atomic_int_set(&engine.render_done, 0);
    g_atomic_int_set(&engine.render_active, 1);
}

void jackdaw_engine_render_tap_stop(void)
{
    g_atomic_int_set(&engine.render_active, 0);
    /* Rings are kept for reuse; freed in jackdaw_engine_quit(). */
}

gboolean jackdaw_engine_render_tap_done(void)
{
    return g_atomic_int_get(&engine.render_done) != 0;
}

size_t jackdaw_engine_render_tap_read(float *L, float *R, size_t max_frames)
{
    if (!engine.render_rb_L || !engine.render_rb_R)
        return 0;
    size_t avL = rt_ringbuffer_read_space(engine.render_rb_L) / sizeof(float);
    size_t avR = rt_ringbuffer_read_space(engine.render_rb_R) / sizeof(float);
    size_t av = avL < avR ? avL : avR;
    if (av > max_frames)
        av = max_frames;
    if (av == 0)
        return 0;
    rt_ringbuffer_read(engine.render_rb_L, (char *)L, av * sizeof(float));
    rt_ringbuffer_read(engine.render_rb_R, (char *)R, av * sizeof(float));
    return av;
}
