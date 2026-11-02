#include <math.h>
#include <string.h>

#include <samplerate.h>
#include <sndfile.h>

#include "host/pluginhost.h"
#include "jackdaw-engine.h"
#include "render.h"
#include "settings.h"
#include "track.h"

/* Render block: small enough to stay within the plugins' max block (the JACK
 * buffer size they were instantiated with), large enough to amortise overhead. */
#define RENDER_BLOCK_MAX 4096
#define RENDER_MIDI_MAX 1024

/* Ported from the Linux JackDAW render.c. libsamplerate is always present on
 * Haiku (as in the feeder), so the HAVE_SAMPLERATE guards are dropped. The
 * master bus here is the project's master_volume/master_muted pair (this port
 * has no master FX track), so the master path applies gain/mute only. */

/* -----------------------------------------------------------------------
 * Format helpers
 * ----------------------------------------------------------------------- */

const char *jackdaw_render_extension(RenderFormat fmt)
{
    switch (fmt) {
        case RENDER_FMT_FLAC:
            return "flac";
        case RENDER_FMT_MP3:
            return "mp3";
        case RENDER_FMT_WAV:
        default:
            return "wav";
    }
}

int jackdaw_render_sf_format(const RenderOptions *o)
{
    switch (o->format) {
        case RENDER_FMT_WAV:
            switch (o->bit_depth) {
                case RENDER_BITS_16:
                    return SF_FORMAT_WAV | SF_FORMAT_PCM_16;
                case RENDER_BITS_24:
                    return SF_FORMAT_WAV | SF_FORMAT_PCM_24;
                case RENDER_BITS_32:
                    return SF_FORMAT_WAV | SF_FORMAT_PCM_32;
            }
            return SF_FORMAT_WAV | SF_FORMAT_PCM_24;
        case RENDER_FMT_FLAC:
            return SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
        case RENDER_FMT_MP3:
            return SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    }
    return 0;
}

gboolean jackdaw_render_format_supported(const RenderOptions *o)
{
    SF_INFO sfi = {0};
    sfi.samplerate = o->sample_rate;
    sfi.channels = o->channels;
    sfi.format = jackdaw_render_sf_format(o);
    return sf_format_check(&sfi) ? TRUE : FALSE;
}

gboolean jackdaw_render_mp3_available(int sample_rate, int channels)
{
    SF_INFO sfi = {0};
    sfi.samplerate = sample_rate > 0 ? sample_rate : 48000;
    sfi.channels = channels > 0 ? channels : 2;
    sfi.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    return sf_format_check(&sfi) ? TRUE : FALSE;
}

void render_options_free_contents(RenderOptions *o)
{
    if (!o)
        return;
    g_free(o->out_path);
    o->out_path = NULL;
    if (o->selected_tracks) {
        g_ptr_array_unref(o->selected_tracks);
        o->selected_tracks = NULL;
    }
}

/* -----------------------------------------------------------------------
 * Settings persistence
 * ----------------------------------------------------------------------- */

void jackdaw_render_options_load(RenderOptions *o)
{
    o->format = (RenderFormat)settings_get_uint32("render_format", RENDER_FMT_WAV);
    o->bit_depth = (RenderBitDepth)settings_get_uint32("render_bit_depth", RENDER_BITS_24);
    o->source = (RenderSource)settings_get_uint32("render_source", RENDER_SRC_MASTER);
    o->method = (RenderMethod)settings_get_uint32("render_method", RENDER_METHOD_OFFLINE);
    o->sample_rate = (int)settings_get_uint32("render_sample_rate", 48000);
    o->channels = (int)settings_get_uint32("render_channels", 2);
    if (o->channels != 1 && o->channels != 2)
        o->channels = 2;
}

void jackdaw_render_options_save(const RenderOptions *o)
{
    settings_set_uint32("render_format", (guint32)o->format);
    settings_set_uint32("render_bit_depth", (guint32)o->bit_depth);
    settings_set_uint32("render_source", (guint32)o->source);
    settings_set_uint32("render_method", (guint32)o->method);
    settings_set_uint32("render_sample_rate", (guint32)o->sample_rate);
    settings_set_uint32("render_channels", (guint32)o->channels);
    if (o->out_path) {
        gchar *dir = g_path_get_dirname(o->out_path);
        settings_set_string("render_last_dir", dir);
        g_free(dir);
    }
    settings_save();
}

/* -----------------------------------------------------------------------
 * Shared helpers
 * ----------------------------------------------------------------------- */

/* Furthest timeline frame across all project tracks (the project end). */
static off_t render_project_end(JackDawProject *p)
{
    off_t maxf = 0;
    guint n = jackdaw_project_track_count(p);
    for (guint i = 0; i < n; i++) {
        off_t tf = jackdaw_track_total_frames(jackdaw_project_get_track(p, i));
        if (tf > maxf)
            maxf = tf;
    }
    return maxf;
}

static gboolean render_track_selected(const RenderOptions *o, JackDawTrack *t)
{
    if (!o->selected_tracks)
        return FALSE;
    for (guint i = 0; i < o->selected_tracks->len; i++)
        if (g_ptr_array_index(o->selected_tracks, i) == t)
            return TRUE;
    return FALSE;
}

/* Resolve the render span [*start, *end) in engine-SR frames. Returns FALSE on
 * success, TRUE if the span is empty/invalid. */
static gboolean render_resolve_span(const RenderOptions *o, off_t *start, off_t *end)
{
    if (o->scope == RENDER_SCOPE_REGION) {
        jackdaw_engine_get_loop_range(start, end);
    } else {
        *start = 0;
        *end = render_project_end(o->project);
    }
    return (*end <= *start);
}

/* Deep-copy options so a worker can outlive the caller's struct. */
static void render_options_copy(RenderOptions *dst, const RenderOptions *src)
{
    *dst = *src;
    dst->out_path = g_strdup(src->out_path);
    if (src->selected_tracks) {
        dst->selected_tracks = g_ptr_array_new();
        for (guint i = 0; i < src->selected_tracks->len; i++)
            g_ptr_array_add(dst->selected_tracks, g_ptr_array_index(src->selected_tracks, i));
    } else {
        dst->selected_tracks = NULL;
    }
}

/* Write `count` frames of L/R to the file, downmixing to mono if channels==1.
 * `inter` must hold count*2 floats. */
static void render_write_frames(SNDFILE *sf, int channels, const float *L, const float *R,
                                long count, float *inter)
{
    if (count <= 0)
        return;
    if (channels == 1) {
        for (long f = 0; f < count; f++)
            inter[f] = (L[f] + R[f]) * 0.5f;
        sf_writef_float(sf, inter, count);
    } else {
        for (long f = 0; f < count; f++) {
            inter[f * 2] = L[f];
            inter[f * 2 + 1] = R[f];
        }
        sf_writef_float(sf, inter, count);
    }
}

/* Clear the internal DSP state (reverb/delay tails, held synth voices) of every
 * plugin the offline render drove, while the engine is still suspended, so the
 * live graph doesn't flush the render's leftover tail as an audible pop. */
static void render_reset_track_chain(JackDawTrack *t)
{
    if (!t)
        return;
    JackDawFxChain *chain = g_atomic_pointer_get(&t->rt_chain);
    if (!chain)
        return;
    for (int i = 0; i < chain->n; i++)
        pluginhost_reset((PluginInstance *)chain->fx[i]);
}

static void render_reset_all_plugins(JackDawProject *proj)
{
    guint n = jackdaw_project_track_count(proj);
    for (guint i = 0; i < n; i++)
        render_reset_track_chain(jackdaw_project_get_track(proj, i));
}

/* -----------------------------------------------------------------------
 * Offline render
 * ----------------------------------------------------------------------- */

typedef struct {
    RenderOptions opt;           /* owned deep copy */
    JackDawRenderProgress *prog; /* borrowed (owned by the UI) */
} OfflineCtx;

/* Parallel per-track processing for one block. The offline render is non-RT, so
 * unlike the live engine this uses a plain GThreadPool with a mutex/cond barrier
 * (no SCHED_FIFO / no-malloc discipline needed). Tracks are independent — each
 * touches only its own plugins, reader, scratch and MIDI buffer — so they fan
 * out across cores; only the master sum stays serial. */
typedef struct {
    JackDawProject *proj;
    const RenderOptions *o;
    EngTrackReader **readers;
    int engine_sr;
    gboolean any_soloed;
    off_t frame;          /* set per block */
    jd_nframes_t nn;      /* set per block */
    float **trkL, **trkR; /* per-track post-fader scratch */
    PhMidiEvent **mev;    /* per-track MIDI gather buffer */
    gboolean *active;     /* OUT: did track i contribute? */
    GMutex mtx;
    GCond cond;
    gint remaining; /* tasks left in the current block */
} RenderPar;

typedef struct {
    RenderPar *par;
    guint idx;
} TrackJob;

/* Process one track into par->trkL[i]/trkR[i], volume+pan applied. Sets
 * active[i] FALSE if the track is gated out (muted / soloed-away / not selected
 * / silent instrument), TRUE if it contributes. */
static void render_process_one_track(RenderPar *par, guint i)
{
    JackDawTrack *t = jackdaw_project_get_track(par->proj, i);
    par->active[i] = FALSE;

    if (par->o->source == RENDER_SRC_SELECTED) {
        if (!render_track_selected(par->o, t))
            return;
    } else {
        gint fl = g_atomic_int_get(&t->state_flags);
        if (fl & TRACK_MUTED)
            return;
        if (par->any_soloed && !(fl & TRACK_SOLOED))
            return;
    }

    jd_nframes_t nn = par->nn;
    float *tmpL = par->trkL[i];
    float *tmpR = par->trkR[i];
    gboolean instr = jackdaw_track_is_instrument(t);
    JackDawFxChain *chain = g_atomic_pointer_get(&t->rt_chain);

    if (instr) {
        if (!chain || chain->n == 0)
            return; /* no instrument -> silent */
        memset(tmpL, 0, nn * sizeof(float));
        memset(tmpR, 0, nn * sizeof(float));
        int nev = eng_gather_render_midi(t, par->frame, nn, par->mev[i], RENDER_MIDI_MAX);
        pluginhost_process_midi((PluginInstance *)chain->fx[0], par->mev[i], nev, tmpL, tmpR,
                                (int)nn);
        for (int fi = 1; fi < chain->n; fi++)
            pluginhost_process((PluginInstance *)chain->fx[fi], tmpL, tmpR, (int)nn);
    } else {
        engine_track_reader_read(par->readers[i], t, par->frame, nn, tmpL, tmpR);
        if (chain)
            for (int fi = 0; fi < chain->n; fi++)
                pluginhost_process((PluginInstance *)chain->fx[fi], tmpL, tmpR, (int)nn);
    }

    gfloat vol = t->volume, pan = t->pan;
    float ang = (pan + 1.0f) * (float)M_PI_4;
    float gL = vol * cosf(ang), gR = vol * sinf(ang);
    for (jd_nframes_t k = 0; k < nn; k++) {
        tmpL[k] *= gL;
        tmpR[k] *= gR;
    }
    par->active[i] = TRUE;
}

static void render_track_worker(gpointer data, gpointer user)
{
    (void)user;
    TrackJob *job = data;
    RenderPar *par = job->par;
    render_process_one_track(par, job->idx);
    g_mutex_lock(&par->mtx);
    if (--par->remaining == 0)
        g_cond_signal(&par->cond);
    g_mutex_unlock(&par->mtx);
}

static gpointer render_offline_thread(gpointer data)
{
    OfflineCtx *ctx = data;
    RenderOptions *o = &ctx->opt;
    JackDawRenderProgress *prog = ctx->prog;
    JackDawProject *proj = o->project;
    gboolean ok = TRUE;

    int engine_sr = (int)jackdaw_engine_get_sample_rate();
    int render_sr = o->sample_rate;
    int block = (int)jackdaw_engine_get_buffer_size();
    if (block > RENDER_BLOCK_MAX)
        block = RENDER_BLOCK_MAX;
    if (block < 64)
        block = 64;

    off_t start = 0, end = 0;
    render_resolve_span(o, &start, &end); /* empty span => writes 0 frames */
    prog->frames_total = end - start;

    SF_INFO sfi = {0};
    sfi.samplerate = render_sr;
    sfi.channels = o->channels;
    sfi.format = jackdaw_render_sf_format(o);
    SNDFILE *sf = sf_open(o->out_path, SFM_WRITE, &sfi);
    if (!sf) {
        g_atomic_int_set(&prog->failed, 1);
        g_atomic_int_set(&prog->finished, 1);
        render_options_free_contents(o);
        g_free(ctx);
        return NULL;
    }
    /* Clip out-of-range floats to [-1,1] on write. The internal mix is float and
     * may legitimately exceed 0 dBFS (plugin output is clamped to +-4, hot amp
     * sims push past unity); integer PCM (WAV/FLAC) would otherwise WRAP those
     * samples into full-scale digital noise. */
    sf_command(sf, SFC_SET_CLIPPING, NULL, SF_TRUE);

    guint ntr = jackdaw_project_track_count(proj);

    /* "any soloed" only matters for the master mix path. */
    gboolean any_soloed = FALSE;
    if (o->source == RENDER_SRC_MASTER) {
        for (guint i = 0; i < ntr; i++)
            if (jackdaw_track_is_soloed(jackdaw_project_get_track(proj, i))) {
                any_soloed = TRUE;
                break;
            }
    }

    /* One reader per audio track (instrument tracks generate from MIDI). */
    EngTrackReader **readers = g_new0(EngTrackReader *, ntr ? ntr : 1);
    for (guint i = 0; i < ntr; i++) {
        JackDawTrack *t = jackdaw_project_get_track(proj, i);
        if (!jackdaw_track_is_instrument(t))
            readers[i] = engine_track_reader_new(t, engine_sr);
    }

    /* Take over the plugin graph for the duration of the render. Wait out any
     * RT block already in flight (which may still touch plugins this cycle)
     * before the worker uses them, then clear any DSP state left by prior live
     * monitoring so the render starts clean. */
    jackdaw_engine_render_suspend(TRUE);
    {
        int bs = (int)jackdaw_engine_get_buffer_size();
        if (bs < 1)
            bs = 1024;
        gulong settle_us = (gulong)((double)bs / engine_sr * 1e6 * 4.0) + 2000;
        g_usleep(settle_us);
    }
    render_reset_all_plugins(proj);

    float *masterL = g_new(float, block);
    float *masterR = g_new(float, block);

    /* Per-track scratch + MIDI buffers so tracks can be processed in parallel. */
    guint nalloc = ntr ? ntr : 1;
    float **trkL = g_new0(float *, nalloc);
    float **trkR = g_new0(float *, nalloc);
    PhMidiEvent **mevbuf = g_new0(PhMidiEvent *, nalloc);
    gboolean *active = g_new0(gboolean, nalloc);
    for (guint i = 0; i < ntr; i++) {
        trkL[i] = g_new(float, block);
        trkR[i] = g_new(float, block);
        mevbuf[i] = g_new(PhMidiEvent, RENDER_MIDI_MAX);
    }

    RenderPar par = {0};
    par.proj = proj;
    par.o = o;
    par.readers = readers;
    par.engine_sr = engine_sr;
    par.any_soloed = any_soloed;
    par.trkL = trkL;
    par.trkR = trkR;
    par.mev = mevbuf;
    par.active = active;
    g_mutex_init(&par.mtx);
    g_cond_init(&par.cond);

    int nthreads = g_get_num_processors();
    if (nthreads < 1)
        nthreads = 1;
    TrackJob *jobs = g_new(TrackJob, nalloc);
    for (guint i = 0; i < ntr; i++) {
        jobs[i].par = &par;
        jobs[i].idx = i;
    }
    GThreadPool *pool = (ntr > 1 && nthreads > 1)
                            ? g_thread_pool_new(render_track_worker, NULL, nthreads, FALSE, NULL)
                            : NULL;

    gboolean need_src = (render_sr != engine_sr);
    double mratio = (double)render_sr / (double)engine_sr;
    size_t outcap = (size_t)((double)block * (mratio > 1.0 ? mratio : 1.0)) + 32;
    float *outL = g_new(float, outcap);
    float *outR = g_new(float, outcap);
    float *inter = g_new(float, outcap * 2);
    SRC_STATE *msrc_L = NULL, *msrc_R = NULL;
    if (need_src) {
        int e = 0;
        msrc_L = src_new(SRC_SINC_BEST_QUALITY, 1, &e);
        msrc_R = src_new(SRC_SINC_BEST_QUALITY, 1, &e);
    }

    gboolean master_muted = proj->master_muted;
    gfloat master_vol = master_muted ? 0.0f : proj->master_volume;

    off_t frame = start;
    while (frame < end && !g_atomic_int_get(&prog->cancel)) {
        jd_nframes_t nn = (end - frame) < block ? (jd_nframes_t)(end - frame) : (jd_nframes_t)block;
        memset(masterL, 0, nn * sizeof(float));
        memset(masterR, 0, nn * sizeof(float));
        pluginhost_set_transport(proj->bpm > 0.0 ? proj->bpm : 120.0, (double)engine_sr,
                                 (gint64)frame, TRUE);

        par.frame = frame;
        par.nn = nn;

        if (pool) {
            par.remaining = (gint)ntr;
            for (guint i = 0; i < ntr; i++)
                g_thread_pool_push(pool, &jobs[i], NULL);
            g_mutex_lock(&par.mtx);
            while (par.remaining > 0)
                g_cond_wait(&par.cond, &par.mtx);
            g_mutex_unlock(&par.mtx);
        } else {
            for (guint i = 0; i < ntr; i++)
                render_process_one_track(&par, i);
        }

        /* Sum per-track contributions in track order. */
        for (guint i = 0; i < ntr; i++) {
            if (!active[i])
                continue;
            const float *tl = trkL[i];
            const float *tr = trkR[i];
            for (jd_nframes_t k = 0; k < nn; k++) {
                masterL[k] += tl[k];
                masterR[k] += tr[k];
            }
        }

        /* Master volume / mute (this port has no master FX track). */
        for (jd_nframes_t k = 0; k < nn; k++) {
            masterL[k] *= master_vol;
            masterR[k] *= master_vol;
        }

        gboolean last = (frame + nn >= end);
        if (need_src && msrc_L && msrc_R) {
            SRC_DATA dl = {.data_in = masterL,
                           .data_out = outL,
                           .input_frames = (long)nn,
                           .output_frames = (long)outcap,
                           .src_ratio = mratio,
                           .end_of_input = last};
            SRC_DATA dr = {.data_in = masterR,
                           .data_out = outR,
                           .input_frames = (long)nn,
                           .output_frames = (long)outcap,
                           .src_ratio = mratio,
                           .end_of_input = last};
            src_process(msrc_L, &dl);
            src_process(msrc_R, &dr);
            long gen = dl.output_frames_gen < dr.output_frames_gen ? dl.output_frames_gen
                                                                   : dr.output_frames_gen;
            render_write_frames(sf, o->channels, outL, outR, gen, inter);
        } else {
            render_write_frames(sf, o->channels, masterL, masterR, (long)nn, inter);
        }

        prog->frames_done = frame - start;
        frame += nn;
    }

    /* Flush any samples the resampler is still holding. */
    if (need_src && msrc_L && msrc_R) {
        while (TRUE) {
            SRC_DATA dl = {.data_in = masterL,
                           .data_out = outL,
                           .input_frames = 0,
                           .output_frames = (long)outcap,
                           .src_ratio = mratio,
                           .end_of_input = 1};
            SRC_DATA dr = {.data_in = masterR,
                           .data_out = outR,
                           .input_frames = 0,
                           .output_frames = (long)outcap,
                           .src_ratio = mratio,
                           .end_of_input = 1};
            src_process(msrc_L, &dl);
            src_process(msrc_R, &dr);
            long gen = dl.output_frames_gen < dr.output_frames_gen ? dl.output_frames_gen
                                                                   : dr.output_frames_gen;
            if (gen <= 0)
                break;
            render_write_frames(sf, o->channels, outL, outR, gen, inter);
        }
        src_delete(msrc_L);
        src_delete(msrc_R);
    }

    /* Return the shared plugins to a clean state before the live graph resumes. */
    render_reset_all_plugins(proj);
    jackdaw_engine_render_suspend(FALSE);

    sf_close(sf);
    if (pool)
        g_thread_pool_free(pool, FALSE, TRUE);
    g_cond_clear(&par.cond);
    g_mutex_clear(&par.mtx);
    for (guint i = 0; i < ntr; i++)
        if (readers[i])
            engine_track_reader_free(readers[i]);
    g_free(readers);
    g_free(masterL);
    g_free(masterR);
    for (guint i = 0; i < ntr; i++) {
        g_free(trkL[i]);
        g_free(trkR[i]);
        g_free(mevbuf[i]);
    }
    g_free(trkL);
    g_free(trkR);
    g_free(mevbuf);
    g_free(active);
    g_free(jobs);
    g_free(outL);
    g_free(outR);
    g_free(inter);

    prog->frames_done = end - start;
    if (!ok)
        g_atomic_int_set(&prog->failed, 1);
    g_atomic_int_set(&prog->finished, 1);

    render_options_free_contents(o);
    g_free(ctx);
    return NULL;
}

GThread *jackdaw_render_offline_start(const RenderOptions *o, JackDawRenderProgress *prog)
{
    OfflineCtx *ctx = g_new0(OfflineCtx, 1);
    render_options_copy(&ctx->opt, o);
    ctx->prog = prog;
    GThread *th = g_thread_new("jackdaw-render", render_offline_thread, ctx);
    if (!th) {
        render_options_free_contents(&ctx->opt);
        g_free(ctx);
        g_atomic_int_set(&prog->failed, 1);
        g_atomic_int_set(&prog->finished, 1);
    }
    return th;
}

/* -----------------------------------------------------------------------
 * Realtime render (master tap + writer thread)
 * ----------------------------------------------------------------------- */

typedef struct {
    RenderOptions opt;
    JackDawRenderProgress *prog;
    JackDawProject *proj;

    SNDFILE *sf;
    int engine_sr, render_sr, channels;
    gboolean need_src;
    SRC_STATE *src_L, *src_R;
    float *rdL, *rdR; /* drained from the tap (engine SR) */
    float *outL, *outR, *inter;
    size_t rd_cap, out_cap;

    off_t start, end;

    /* transport state to restore */
    gboolean saved_loop_enabled;
    int saved_record_mode;
    /* per-track solo state to restore (SRC_SELECTED) */
    gboolean *saved_solo;
    guint saved_solo_n;

    GThread *writer;
    volatile gint writer_stop;
    volatile gint writer_done;
} RealtimeCtx;

static RealtimeCtx *g_rt; /* at most one realtime render at a time */

/* Drain `frames` from rdL/rdR through optional SR conversion to the file. */
static void rt_write_drained(RealtimeCtx *c, long frames, gboolean last)
{
    if (c->need_src && c->src_L && c->src_R) {
        SRC_DATA dl = {.data_in = c->rdL,
                       .data_out = c->outL,
                       .input_frames = frames,
                       .output_frames = (long)c->out_cap,
                       .src_ratio = (double)c->render_sr / c->engine_sr,
                       .end_of_input = last};
        SRC_DATA dr = {.data_in = c->rdR,
                       .data_out = c->outR,
                       .input_frames = frames,
                       .output_frames = (long)c->out_cap,
                       .src_ratio = (double)c->render_sr / c->engine_sr,
                       .end_of_input = last};
        src_process(c->src_L, &dl);
        src_process(c->src_R, &dr);
        long gen = dl.output_frames_gen < dr.output_frames_gen ? dl.output_frames_gen
                                                               : dr.output_frames_gen;
        render_write_frames(c->sf, c->channels, c->outL, c->outR, gen, c->inter);
        return;
    }
    render_write_frames(c->sf, c->channels, c->rdL, c->rdR, frames, c->inter);
}

static gpointer render_rt_writer(gpointer data)
{
    RealtimeCtx *c = data;
    while (!g_atomic_int_get(&c->writer_stop)) {
        size_t got = jackdaw_engine_render_tap_read(c->rdL, c->rdR, c->rd_cap);
        if (got > 0)
            rt_write_drained(c, (long)got, FALSE);
        else
            g_usleep(2000); /* 2 ms */
    }
    /* Final drain after stop is signalled. */
    while (TRUE) {
        size_t got = jackdaw_engine_render_tap_read(c->rdL, c->rdR, c->rd_cap);
        if (got == 0)
            break;
        rt_write_drained(c, (long)got, FALSE);
    }
    if (c->need_src && c->src_L && c->src_R)
        rt_write_drained(c, 0, TRUE); /* flush resampler tail */
    g_atomic_int_set(&c->writer_done, 1);
    return NULL;
}

gboolean jackdaw_render_realtime_start(const RenderOptions *o, JackDawRenderProgress *prog)
{
    if (g_rt)
        return TRUE; /* already running */

    RealtimeCtx *c = g_new0(RealtimeCtx, 1);
    render_options_copy(&c->opt, o);
    c->prog = prog;
    c->proj = o->project;
    c->engine_sr = (int)jackdaw_engine_get_sample_rate();
    c->render_sr = o->sample_rate;
    c->channels = o->channels;
    c->need_src = (c->render_sr != c->engine_sr);

    if (render_resolve_span(o, &c->start, &c->end)) {
        render_options_free_contents(&c->opt);
        g_free(c);
        return TRUE; /* empty span */
    }
    prog->frames_total = c->end - c->start;

    SF_INFO sfi = {0};
    sfi.samplerate = c->render_sr;
    sfi.channels = c->channels;
    sfi.format = jackdaw_render_sf_format(o);
    c->sf = sf_open(o->out_path, SFM_WRITE, &sfi);
    if (!c->sf) {
        render_options_free_contents(&c->opt);
        g_free(c);
        return TRUE;
    }
    sf_command(c->sf, SFC_SET_CLIPPING, NULL, SF_TRUE);

    c->rd_cap = RENDER_BLOCK_MAX;
    double rat = (double)c->render_sr / c->engine_sr;
    c->out_cap = (size_t)((double)c->rd_cap * (rat > 1.0 ? rat : 1.0)) + 32;
    c->rdL = g_new(float, c->rd_cap);
    c->rdR = g_new(float, c->rd_cap);
    c->outL = g_new(float, c->out_cap);
    c->outR = g_new(float, c->out_cap);
    c->inter = g_new(float, c->out_cap * 2);
    if (c->need_src) {
        int e = 0;
        c->src_L = src_new(SRC_SINC_BEST_QUALITY, 1, &e);
        c->src_R = src_new(SRC_SINC_BEST_QUALITY, 1, &e);
    }

    /* Looping/punch must not interfere with a straight-through render. */
    c->saved_loop_enabled = jackdaw_engine_get_loop_enabled();
    c->saved_record_mode = jackdaw_engine_get_record_mode();
    jackdaw_engine_set_loop_enabled(FALSE);
    jackdaw_engine_set_record_mode(RECORD_MODE_NORMAL);

    /* Selected-tracks submix: solo the set so only it reaches master (reuses the
     * live RT solo gating). Saved + restored on teardown. */
    if (o->source == RENDER_SRC_SELECTED) {
        guint n = jackdaw_project_track_count(c->proj);
        c->saved_solo = g_new0(gboolean, n ? n : 1);
        c->saved_solo_n = n;
        for (guint i = 0; i < n; i++) {
            JackDawTrack *t = jackdaw_project_get_track(c->proj, i);
            c->saved_solo[i] = jackdaw_track_is_soloed(t);
            jackdaw_track_set_soloed(t, render_track_selected(o, t));
        }
    }

    g_rt = c;

    jackdaw_engine_locate(c->start);
    jackdaw_engine_render_tap_start(c->end);
    c->writer = g_thread_new("jackdaw-render-rt", render_rt_writer, c);
    jackdaw_engine_start_playback();
    return FALSE;
}

static void render_rt_teardown(RealtimeCtx *c)
{
    jackdaw_engine_stop_playback();
    jackdaw_engine_render_tap_stop();

    g_atomic_int_set(&c->writer_stop, 1);
    if (c->writer)
        g_thread_join(c->writer);

    if (c->sf)
        sf_close(c->sf);
    if (c->src_L)
        src_delete(c->src_L);
    if (c->src_R)
        src_delete(c->src_R);

    /* Restore transport + solo state. */
    jackdaw_engine_set_loop_enabled(c->saved_loop_enabled);
    jackdaw_engine_set_record_mode(c->saved_record_mode);
    if (c->saved_solo) {
        for (guint i = 0; i < c->saved_solo_n; i++)
            jackdaw_track_set_soloed(jackdaw_project_get_track(c->proj, i), c->saved_solo[i]);
        g_free(c->saved_solo);
    }

    g_free(c->rdL);
    g_free(c->rdR);
    g_free(c->outL);
    g_free(c->outR);
    g_free(c->inter);
    render_options_free_contents(&c->opt);
    g_free(c);
}

void jackdaw_render_realtime_poll(JackDawRenderProgress *prog)
{
    RealtimeCtx *c = g_rt;
    if (!c)
        return;

    off_t pos = jackdaw_engine_get_play_pos();
    off_t dn = pos - c->start;
    if (dn < 0)
        dn = 0;
    prog->frames_done = dn;

    gboolean cancel = g_atomic_int_get(&prog->cancel) != 0;
    if (cancel || jackdaw_engine_render_tap_done()) {
        prog->frames_done = c->end - c->start;
        g_rt = NULL;
        render_rt_teardown(c);
        /* On cancel the UI already knows (it set prog->cancel); leave `failed`
         * clear so cancel and error stay distinguishable. */
        g_atomic_int_set(&prog->finished, 1);
    }
}
