/* jack_backend.c — the JACK implementation of audio_backend.h.
 *
 * A thin layer over the JACK client API: one client, opened with
 * JackNoStartServer, so JackDAW only ever joins a server the user started
 * (from Jack Graph or jackd). JdPort is the jack_port_t itself, and the RT
 * calls are direct forwards, so the process path costs one indirect call per
 * port buffer.
 *
 * The client handle is written by open/close on the engine's non-RT caller
 * and read by every other call. The engine opens the client before anything
 * else touches it and closes it after its threads have stopped. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/thread.h>

#include "engine/audio_backend.h"

static jack_client_t *jb_client;
static JdBackendCallbacks jb_cb;
static void *jb_arg;

/* ---- JACK callbacks -> engine callbacks ---- */

static int jb_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    return jb_cb.process ? jb_cb.process(nframes, jb_arg) : 0;
}

static int jb_buffer_size(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    return jb_cb.buffer_size ? jb_cb.buffer_size(nframes, jb_arg) : 0;
}

static void jb_thread_init(void *arg)
{
    (void)arg;
    if (jb_cb.thread_init)
        jb_cb.thread_init(jb_arg);
}

static int jb_xrun(void *arg)
{
    (void)arg;
    if (jb_cb.xrun)
        jb_cb.xrun(jb_arg);
    return 0;
}

static void jb_shutdown(void *arg)
{
    (void)arg;
    if (jb_cb.shutdown)
        jb_cb.shutdown(jb_arg);
}

static void jb_port_registration(jack_port_id_t port, int registered, void *arg)
{
    (void)port;
    (void)registered;
    (void)arg;
    if (jb_cb.ports_changed)
        jb_cb.ports_changed(jb_arg);
}

static void jb_port_connect(jack_port_id_t a, jack_port_id_t b, int connect, void *arg)
{
    (void)a;
    (void)b;
    (void)connect;
    (void)arg;
    if (jb_cb.connections_changed)
        jb_cb.connections_changed(jb_arg);
}

/* ---- JdBackend ---- */

static int jb_open(const char *client_name, const JdBackendCallbacks *cb, void *arg, char *err,
                   size_t err_len)
{
    jack_status_t status;

    if (jb_client)
        return 0;
    if (cb)
        jb_cb = *cb;
    else
        memset(&jb_cb, 0, sizeof jb_cb);
    jb_arg = arg;

    /* JackNoStartServer: connect only to the server the user already started
     * (e.g. from Jack Graph's settings), never spawn or reconfigure one of our
     * own. If no server is running this fails cleanly. */
    jb_client = jack_client_open(client_name, JackNoStartServer, &status);
    if (!jb_client) {
        if (err && err_len)
            snprintf(err, err_len,
                     "Could not connect to the JACK server.\n"
                     "Start JACK first (e.g. from Jack Graph), then launch JackDAW.");
        return -1;
    }

    jack_set_thread_init_callback(jb_client, jb_thread_init, NULL);
    jack_set_process_callback(jb_client, jb_process, NULL);
    jack_set_buffer_size_callback(jb_client, jb_buffer_size, NULL);
    jack_set_xrun_callback(jb_client, jb_xrun, NULL);
    jack_on_shutdown(jb_client, jb_shutdown, NULL);
    jack_set_port_registration_callback(jb_client, jb_port_registration, NULL);
    jack_set_port_connect_callback(jb_client, jb_port_connect, NULL);
    return 0;
}

static int jb_activate(void)
{
    return jb_client ? jack_activate(jb_client) : -1;
}

static void jb_close(void)
{
    if (!jb_client)
        return;
    jack_deactivate(jb_client);
    jack_client_close(jb_client);
    jb_client = NULL;
}

static jd_nframes_t jb_sample_rate(void)
{
    return jb_client ? jack_get_sample_rate(jb_client) : 0;
}

static jd_nframes_t jb_buffer_size_get(void)
{
    return jb_client ? jack_get_buffer_size(jb_client) : 0;
}

static JdPort *jb_port_register(const char *short_name, int type, int direction)
{
    if (!jb_client)
        return NULL;
    const char *jtype = (type == JD_PORT_MIDI) ? JACK_DEFAULT_MIDI_TYPE : JACK_DEFAULT_AUDIO_TYPE;
    unsigned long flags = (direction & JD_PORT_INPUT) ? JackPortIsInput : JackPortIsOutput;
    return (JdPort *)jack_port_register(jb_client, short_name, jtype, flags, 0);
}

static void jb_port_unregister(JdPort *port)
{
    if (jb_client && port)
        jack_port_unregister(jb_client, (jack_port_t *)port);
}

static const char *jb_port_name(JdPort *port)
{
    return port ? jack_port_name((jack_port_t *)port) : NULL;
}

static jd_nframes_t jb_port_capture_latency(JdPort *port)
{
    jack_latency_range_t lr = {0, 0};
    if (port)
        jack_port_get_latency_range((jack_port_t *)port, JackCaptureLatency, &lr);
    return lr.max;
}

static void *jb_port_buffer(JdPort *port, jd_nframes_t nframes)
{
    return jack_port_get_buffer((jack_port_t *)port, nframes);
}

static uint32_t jb_midi_event_count(void *port_buffer)
{
    return jack_midi_get_event_count(port_buffer);
}

static int jb_midi_event_get(JdMidiEvent *event, void *port_buffer, uint32_t index)
{
    jack_midi_event_t ev;
    int r = jack_midi_event_get(&ev, port_buffer, index);
    if (r != 0)
        return r;
    event->time = ev.time;
    event->size = ev.size;
    event->buffer = ev.buffer;
    return 0;
}

static void jb_midi_clear(void *port_buffer)
{
    jack_midi_clear_buffer(port_buffer);
}

static int jb_midi_event_write(void *port_buffer, jd_nframes_t time, const uint8_t *data,
                               size_t size)
{
    return jack_midi_event_write(port_buffer, time, data, size);
}

static const char **jb_get_ports(int type, int flags)
{
    if (!jb_client)
        return NULL;
    const char *jtype = (type == JD_PORT_MIDI) ? JACK_DEFAULT_MIDI_TYPE : JACK_DEFAULT_AUDIO_TYPE;
    unsigned long jflags = 0;
    if (flags & JD_PORT_INPUT)
        jflags |= JackPortIsInput;
    if (flags & JD_PORT_OUTPUT)
        jflags |= JackPortIsOutput;
    if (flags & JD_PORT_PHYSICAL)
        jflags |= JackPortIsPhysical;
    return jack_get_ports(jb_client, NULL, jtype, jflags);
}

static void jb_free_ports(const char **ports)
{
    if (ports)
        jack_free((void *)ports);
}

static int jb_connect(const char *source, const char *destination)
{
    if (!jb_client)
        return -1;
    int r = jack_connect(jb_client, source, destination);
    return (r == EEXIST) ? 0 : r;
}

static int jb_disconnect(const char *source, const char *destination)
{
    return jb_client ? jack_disconnect(jb_client, source, destination) : -1;
}

/* Worker threads get JACK's own RT priority, the way jack_client_create_thread
 * gives it to clients' helper threads: a plain pthread would be preempted and
 * make xruns worse. jack_native_thread_t is a pthread_t on Haiku (posix). */
static int jb_create_rt_thread(pthread_t *thread, void *(*fn)(void *), void *arg)
{
    if (!jb_client)
        return -1;
    int prio = jack_client_real_time_priority(jb_client);
    int rt = (prio > 0) ? 1 : 0;
    return jack_client_create_thread(jb_client, thread, prio, rt, fn, arg);
}

static const JdBackend jb_backend = {
    JD_BACKEND_ABI,
    "JACK",
    jb_open,
    jb_activate,
    jb_close,
    jb_sample_rate,
    jb_buffer_size_get,
    jb_port_register,
    jb_port_unregister,
    jb_port_name,
    jb_port_capture_latency,
    jb_port_buffer,
    jb_midi_event_count,
    jb_midi_event_get,
    jb_midi_clear,
    jb_midi_event_write,
    jb_get_ports,
    jb_free_ports,
    jb_connect,
    jb_disconnect,
    jb_create_rt_thread,
    NULL, /* hold_latency: JACK's port latency does not move */
};

const JdBackend *jackdaw_backend_entry(uint32_t abi)
{
    return (abi == JD_BACKEND_ABI) ? &jb_backend : NULL;
}
