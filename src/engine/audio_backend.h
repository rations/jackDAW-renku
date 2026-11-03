/* audio_backend.h — the engine's view of the audio and MIDI system.
 *
 * The engine (jackdaw-engine.c) talks to the audio system only through this
 * table, so the same engine runs on JACK or on another backend. The calls
 * mirror the subset of the JACK client API the engine was written against:
 * one client with named mono audio ports and MIDI ports, a process callback
 * that is handed port buffers each cycle, and connections between ports named
 * by string. A backend that is not JACK presents its own endpoints the same way.
 *
 * Threading: open, activate, close, port_register/unregister, port_name,
 * port_capture_latency, get_ports, connect and disconnect are non-RT and are
 * called from the engine's single non-RT caller (see jackdaw-engine.h).
 * port_buffer and the midi_* calls are RT and are called only from the
 * process callback's thread, as jack_port_get_buffer requires. The callbacks
 * in JdBackendCallbacks follow the JACK rules: process and buffer_size may run
 * on the RT thread; the others run on notification threads and must only post.
 *
 * Plain C, no glib, so a backend can be built as a separate add-on. */
#ifndef JACKDAW_AUDIO_BACKEND_H
#define JACKDAW_AUDIO_BACKEND_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frames in one process cycle (JACK's jack_nframes_t). */
typedef uint32_t jd_nframes_t;

/* A port of this client. Opaque; owned by the backend. */
typedef struct JdPort JdPort;

/* Port types. */
enum { JD_PORT_AUDIO = 0, JD_PORT_MIDI = 1 };

/* Port direction / selection flags, as JackPortIsInput, JackPortIsOutput and
 * JackPortIsPhysical: a hardware capture port is an OUTPUT (you connect from
 * it), a hardware playback port is an INPUT. */
enum { JD_PORT_INPUT = 1 << 0, JD_PORT_OUTPUT = 1 << 1, JD_PORT_PHYSICAL = 1 << 2 };

/* One MIDI event in a port buffer (jack_midi_event_t). buffer points into the
 * port buffer and is valid for the current cycle only. */
typedef struct {
    jd_nframes_t time; /* frame offset in the cycle */
    size_t size;
    uint8_t *buffer;
} JdMidiEvent;

typedef struct {
    /* RT: one cycle of nframes. Return 0. */
    int (*process)(jd_nframes_t nframes, void *arg);
    /* The cycle length changed (not during a process call). Return 0. */
    int (*buffer_size)(jd_nframes_t nframes, void *arg);
    /* Once on each RT thread the backend starts, before its first cycle. */
    void (*thread_init)(void *arg);
    /* A cycle missed its deadline. */
    void (*xrun)(void *arg);
    /* The audio system went away; the client is no longer running. */
    void (*shutdown)(void *arg);
    /* Some port in the system appeared or went away. */
    void (*ports_changed)(void *arg);
    /* Some connection in the system was made or broken. */
    void (*connections_changed)(void *arg);
} JdBackendCallbacks;

#define JD_BACKEND_ABI 2

/* A backend that changes its period while running (the buffer_size callback)
 * goes no higher than its period at open or this, whichever is longer; the
 * engine and plug-ins are sized for that up front. JACK can be set to any
 * period, and a longer one than this reallocates the engine's buffers in the
 * callback, as JACK allows. */
#define JD_MAX_PERIOD 4096

typedef struct JdBackend {
    uint32_t abi; /* JD_BACKEND_ABI */
    const char *name;

    /* Open the client and install the callbacks (not yet running). 0 = ok;
     * otherwise a message for the user is written to err. */
    int (*open)(const char *client_name, const JdBackendCallbacks *cb, void *arg, char *err,
                size_t err_len);
    /* Start cycles. 0 = ok. */
    int (*activate)(void);
    /* Stop cycles and close the client. Every port goes with it. */
    void (*close)(void);

    jd_nframes_t (*sample_rate)(void);
    jd_nframes_t (*buffer_size)(void);

    /* Register a port named short_name. type JD_PORT_AUDIO/MIDI, direction
     * JD_PORT_INPUT or JD_PORT_OUTPUT. NULL on failure. */
    JdPort *(*port_register)(const char *short_name, int type, int direction);
    void (*port_unregister)(JdPort *port);
    /* Full name ("client:port"), as get_ports and connect use. */
    const char *(*port_name)(JdPort *port);
    /* Largest capture latency of an input port, in frames. */
    jd_nframes_t (*port_capture_latency)(JdPort *port);

    /* RT, process thread only. */
    void *(*port_buffer)(JdPort *port, jd_nframes_t nframes);
    uint32_t (*midi_event_count)(void *port_buffer);
    int (*midi_event_get)(JdMidiEvent *event, void *port_buffer, uint32_t index); /* 0 = ok */
    void (*midi_clear)(void *port_buffer);
    int (*midi_event_write)(void *port_buffer, jd_nframes_t time, const uint8_t *data,
                            size_t size); /* 0 = ok */

    /* NULL-terminated full names of the system's ports of `type` whose flags
     * include all of `flags`; NULL if none. Free with free_ports. */
    const char **(*get_ports)(int type, int flags);
    void (*free_ports)(const char **ports);
    /* 0 = ok (an existing connection counts as ok for connect). */
    int (*connect)(const char *source, const char *destination);
    int (*disconnect)(const char *source, const char *destination);

    /* Start a worker thread with the backend's RT scheduling, joinable with
     * pthread_join. 0 = ok. */
    int (*create_rt_thread)(pthread_t *thread, void *(*fn)(void *), void *arg);

    /* Optional, NULL if the backend has nothing to hold. While hold is
     * nonzero, keep every input's capture latency as it is: a take is placed
     * by the latency sampled when it was armed, so a change during the take
     * would move the rest of it. A backend that would otherwise add margin
     * after an input runs dry drops the late input instead. Non-RT. */
    void (*hold_latency)(int hold);
} JdBackend;

/* Engine side: the backend named `which` ("jack", the default, or "mediakit").
 * NULL, with a message for the user in err, if it is not available. */
const JdBackend *audio_backend_load(const char *which, char *err, size_t err_len);

/* The same for another application: the JACK add-on is looked for in
 * $<env_var>/jack.so, backends/jack.so next to the executable, and
 * <app_name>/jack.so in the add-ons directories; messages name app_name. */
const JdBackend *audio_backend_load_for(const char *which, const char *app_name,
                                        const char *env_var, char *err, size_t err_len);

/* Entry point a backend exports (looked up by name when it is an add-on).
 * Returns NULL if it does not speak `abi`. */
typedef const JdBackend *(*JdBackendEntryFn)(uint32_t abi);
#define JD_BACKEND_ENTRY_SYMBOL "jackdaw_backend_entry"

/* The built-in Media Kit backend (src/backend/mediakit/mediakit_backend.cpp).
 * Other hosts that build that file in (VST3-renku's vst3host) use it through
 * these two calls as well. */
const JdBackend *jackdaw_mediakit_backend_entry(uint32_t abi);

/* The name of the media node, as Cortex shows it; its MIDI endpoints are
 * "<name> MIDI in N" and so on. "JackDAW" unless set. Only takes effect if
 * called before open. Non-RT. */
void jackdaw_mediakit_set_node_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* JACKDAW_AUDIO_BACKEND_H */
