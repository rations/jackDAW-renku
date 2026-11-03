/* mediakit_backend.cpp — the RenkuOS Media Kit implementation of audio_backend.h.
 *
 * JackDAW becomes one media node, "JackDAW", that other nodes connect to and
 * that the user patches in Cortex. It is a BBufferProducer and a
 * BBufferConsumer driven by its own BMediaEventLooper, in the way
 * SoundPlayNode (src/kits/media/SoundPlayNode.cpp) drives BSoundPlayer:
 * one event per period, scheduled on the time source, each running one
 * engine cycle and sending one buffer to every connected output.
 *
 * Engine ports and Media Kit connections. The engine was written for JACK:
 * mono ports that it reads and writes each cycle, connected by name. Here a
 * port is a mono buffer owned by the backend, and media connections carry
 * several channels each.
 *  - Outputs: the engine's out_1/out_2 make the stereo jack "Master", out_3/
 *    out_4 "Out 3+4" and so on, and "metronome" the mono jack "Metronome".
 *    A media output connects to exactly one input, so every jack always
 *    offers one more free output: the same mix can feed the system mixer and
 *    a hardware input at once.
 *  - Inputs: the node always offers a free input, and each connection to it
 *    (from a hardware capture output, or anything else patched in Cortex)
 *    becomes a numbered input with its own FIFO. The engine's capture ports
 *    (in_N) read one channel of one input each. The engine's connect(source,
 *    in_N) makes the media connection when needed and then routes the chosen
 *    channel, which is what jack_connect does for JACK.
 *  - The master auto-connects to the system mixer when the node starts, like
 *    MediaPlayer's audio producer, so JackDAW is heard alongside other apps.
 *
 * Clock. The rate and period are those of the system mixer's output, which
 * are the hardware's (multi_audio only accepts its driver's own rate and
 * buffer length), and the node is slaved to the system time source. A
 * connection at another rate or buffer length, or to a consumer on another
 * time source, is refused. Inputs from a
 * second device run on that device's clock; their FIFO absorbs jitter and
 * drops or repeats data on drift, which is audible.
 *
 * Period changes (multi_audio's "Buffer size" setting, changed while
 * running). Neither the system mixer nor multi_audio accepts ChangeFormat()
 * from a producer, and the Media Kit sends no format-change notice, so:
 *  - A hardware input that JackDAW feeds directly asks for its new length
 *    itself (FormatChangeRequested); it is accepted when every other output
 *    goes to that same device or to the system mixer.
 *  - The system mixer, when its output changes, tells its producers of its
 *    new latency (LatencyChanged). The roster watcher then reads the mixer's
 *    output format, and the node follows its period when all its outputs go
 *    to the mixer. The mixer reads each buffer's size_used, but sizes its
 *    ring for an input from the length in the connection's format (three
 *    of them, or two output buffers); a longer buffer overruns it, and it
 *    crashed the mixer in testing. So a consumer that leaves the length
 *    open (the mixer does) is given the longest period we may change to,
 *    and every buffer sent is one period, within it.
 * The engine's mix buffers, the port buffers and the input FIFOs are sized
 * for the longest period up front (JD_MAX_PERIOD, or the period at open if
 * longer), so a change allocates nothing but the new buffer groups, as a
 * connection does. A change during a take moves the rest of the take: the
 * device restarts its streams and the capture latency changes with it.
 *
 * Threads.
 *  - The node's control thread (BMediaEventLooper, B_URGENT_PRIORITY, the
 *    looper default, so the system mixer and multi_audio at
 *    B_REAL_TIME_PRIORITY always preempt it) runs the engine cycle, receives
 *    capture buffers, and handles every connection hook. Those all run on
 *    one thread, so the cycle never races a connection change.
 *  - The engine's non-RT caller (the main window) registers ports, routes
 *    capture channels and asks for connections. Port slots and routes are
 *    published with atomics. Port buffers are never freed while the client
 *    is open (a slot is reused), so the cycle can always read a slot it saw.
 *    The input table is guarded by fInputLock for that caller.
 *  - A small looper watches the roster and turns its notifications into the
 *    engine's ports/connections/shutdown callbacks.
 *
 * Allocation: buffers and FIFOs are allocated when a port is registered or a
 * connection is made (the Media Kit runs connection hooks on the control
 * thread, as for every node), and buffer groups also when the period
 * changes; never per cycle. Buffer requests do not wait. */
#include <Autolock.h>
#include <Buffer.h>
#include <BufferConsumer.h>
#include <BufferGroup.h>
#include <BufferProducer.h>
#include <Locker.h>
#include <Looper.h>
#include <MediaDefs.h>
#include <MediaEventLooper.h>
#include <MediaNode.h>
#include <MediaRoster.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <OS.h>
#include <TimeSource.h>

#include <atomic>
#include <math.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "engine/audio_backend.h"

namespace
{

const int32 kMaxPorts = 256;
const int32 kMaxJacks = 34;      // Master, Out 3+4 ... Out 63+64, Metronome
const int32 kMaxOutConns = 8;    // connections per jack
const int32 kMaxInputs = 16;     // input connections
const int32 kMaxInChannels = 64; // channels per input connection
const int32 kMidiMaxEvents = 256;
const int32 kMetronomeJack = kMaxJacks - 1;
const int32 kFifoPeriods = 8; // FIFO capacity, in periods
// An input is read once it holds this much (in half periods): a period and a
// half at first. One period, the least that lets a cycle run at all, ran dry
// once within seconds of connecting a UMC204HD at 1 ms -- a gap at the start
// of the first take -- and then held. Each time it runs dry the wait grows by
// half a period, up to two, so more margin for a device's delivery jitter is
// only paid where it is needed.
const int32 kPrimeHalvesFirst = 3;
const int32 kPrimeHalvesMax = 4;
// The capture latency reported for a take is the largest offset seen over
// this long (see _FillInputs).
const bigtime_t kLatencyWindowUs = 250000;

const char *kClientPrefix = "jackdaw:";
const char *kCapturePrefix = "media:";

enum { SEND_CYCLE_EVENT = BTimedEventQueue::B_USER_EVENT + 1 };
enum {
    MSG_DIAG_REPORT = 'jdmd',
    MSG_CHECK_PERIOD = 'jdck',   // watcher: read the system mixer's period
    MSG_PERIOD_CHANGED = 'jdpc', // watcher: report a period change
};
// Written to the node's control port by the roster watcher; handled in
// JdMediaNode::HandleMessage. Media Kit codes are 0x100-0x7ff
// (ServerInterface.h), so a four-letter code cannot collide with them.
const int32 kFollowMixerCode = 'jdfm';
struct FollowMixer {
    float rate;
    int32 period;
    port_id mixerPort; // the mixer's control port: its inputs' destinations
};
// The control thread asks the watcher for a check at most once at a time.
std::atomic<int32> gCheckQueued{0};
const int32 kMinPeriod = 16;

/* JACKDAW_DIAG counters: written by the control thread with relaxed atomics
 * (no lock, no allocation), read and printed every 2 s by the roster
 * watcher's thread. */
struct Diag {
    std::atomic<int32> sent{0};              // buffers handed to SendBuffer
    std::atomic<int32> sendFail{0};          // SendBuffer refused them
    std::atomic<int32> noBuffer{0};          // the buffer group was empty
    std::atomic<int32> lateNotes{0};         // LateNoticeReceived
    std::atomic<int32> lateUs{0};            // largest late notice, usecs
    std::atomic<int32> peakMilli{0};         // largest master sample, x1000
    std::atomic<int32> minLeadUs{INT32_MAX}; // least time between send and play
    std::atomic<int32> lateWake{0};          // cycles woken a period late
    // Capture, for the first input that delivers a buffer on our time
    // source: does it keep pace with the cycle?
    std::atomic<int32> capInput{-1};    // which input is measured
    std::atomic<int32> capBufs{0};      // buffers received (this report)
    std::atomic<int32> capGaps{0};      // stamp stepped > 1.5 buffers (this report)
    std::atomic<int32> capMaxStepUs{0}; // largest stamp step (this report)
    std::atomic<int32> capDry{0};       // input ran dry in a cycle (this report)
    std::atomic<int32> capLatMin{INT32_MAX}, capLatMax{0}; // its offset, frames (this report)
    std::atomic<int64> capFrames{0};    // frames since the first stamp
    std::atomic<int64> capSpanUs{0};    // last stamp - first stamp
    std::atomic<int64> capCycles0{0};   // fCycles at the first stamp
    bigtime_t capFirst = 0, capPrev = 0; // control thread only
};
Diag gDiag;
bool gDiagOn = false;

struct MidiBuffer {
    uint32 count;
    JdMidiEvent events[kMidiMaxEvents];
    uint8 data[kMidiMaxEvents][4];
};

struct Port {
    std::atomic<int32> used{0}; // 1 while registered (release/acquire)
    int type = JD_PORT_AUDIO;
    int direction = JD_PORT_INPUT;
    char shortName[64] = "";
    char fullName[80] = "";
    float *audio = NULL;     // MaxPeriod() frames, kept until close
    MidiBuffer *midi = NULL; // kept until close
    // Inputs: (generation << 16) | (input << 8) | channel, or -1.
    std::atomic<int32> route{-1};
};

struct OutConn {
    bool reserved = false; // PrepareToConnect .. Disconnect
    bool connected = false;
    bool enabled = true;
    media_output output;
    BBufferGroup *group = NULL;
    bool ownGroup = false; // group is ours (not the consumer's)
    // The consumer chose the buffer length (a hardware input: exactly its
    // own); otherwise the format carries the longest we may send.
    bool fixedLength = false;
    bigtime_t groupLead = 0; // how far ahead of play our group has buffers for
    bigtime_t latency = 0; // downstream
};

struct Jack {
    std::atomic<int32> active{0};
    char name[32] = "";
    int32 channels = 0;
    std::atomic<int32> portSlot[2] = {{-1}, {-1}};
    OutConn conns[kMaxOutConns];
};

struct Input {
    bool connected = false;
    uint8 generation = 0;
    bool madeByUs = false;   // connect() made it; disconnect may undo it
    media_node producerNode; // valid when madeByUs
    media_input input;
    int32 channels = 0;
    float *fifo = NULL;           // interleaved float, capFrames frames
    uint32 capFrames = 0;         // power of two
    uint64 written = 0, read = 0; // frames; control thread only
    float *scratch = NULL;        // this cycle, planar: channels x period (max period long)
    bool primed = false;
    int32 primeHalves = kPrimeHalvesFirst; // fill to start reading at
    bigtime_t lastEnd = 0;                 // performance time after the newest frame
    uint64 owed = 0;                       // frames of cycles that ran dry, still to drop
    int32 latCur = -1, latPrev = -1;       // largest offset, this and the last window
    int32 latCycles = 0;                   // cycles into this window
    std::atomic<int32> latencyFrames{0};
};

// Set by the engine while a take is open (JdBackend::hold_latency).
std::atomic<int32> gHoldLatency{0};

bool format_is_convertible(uint32 f)
{
    return f == media_raw_audio_format::B_AUDIO_FLOAT ||
           f == media_raw_audio_format::B_AUDIO_DOUBLE ||
           f == media_raw_audio_format::B_AUDIO_INT || f == media_raw_audio_format::B_AUDIO_SHORT;
}

size_t sample_size(uint32 f)
{
    return f & media_raw_audio_format::B_AUDIO_SIZE_MASK;
}

inline float clampf(float v)
{
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

void write_sample(char *dst, uint32 f, float v)
{
    switch (f) {
        case media_raw_audio_format::B_AUDIO_FLOAT:
            *(float *)dst = v;
            break;
        case media_raw_audio_format::B_AUDIO_DOUBLE:
            *(double *)dst = v;
            break;
        case media_raw_audio_format::B_AUDIO_INT:
            *(int32 *)dst = (int32)lrintf(clampf(v) * 2147483392.0f);
            break;
        case media_raw_audio_format::B_AUDIO_SHORT:
            *(int16 *)dst = (int16)lrintf(clampf(v) * 32767.0f);
            break;
    }
}

float read_sample(const char *src, uint32 f)
{
    switch (f) {
        case media_raw_audio_format::B_AUDIO_FLOAT:
            return *(const float *)src;
        case media_raw_audio_format::B_AUDIO_DOUBLE:
            return (float)*(const double *)src;
        case media_raw_audio_format::B_AUDIO_INT:
            return (float)*(const int32 *)src * (1.0f / 2147483648.0f);
        case media_raw_audio_format::B_AUDIO_SHORT:
            return (float)*(const int16 *)src * (1.0f / 32768.0f);
    }
    return 0.0f;
}

uint32 next_pow2(uint32 v)
{
    uint32 p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

class JdMediaNode;

/* Receives roster notifications on its own thread and forwards them to the
 * engine's callbacks, which only post to the UI. */
class RosterWatcher : public BLooper
{
public:
    RosterWatcher() : BLooper("JackDAW media watcher")
    {
    }
    ~RosterWatcher() override
    {
        delete fDiagRunner;
    }
    void MessageReceived(BMessage *message) override;
    BMessageRunner *fDiagRunner = NULL;
};

class JdMediaNode : public BBufferConsumer, public BBufferProducer, public BMediaEventLooper
{
public:
    JdMediaNode(float rate, int32 period);
    ~JdMediaNode() override;

    // BMediaNode
    BMediaAddOn *AddOn(int32 *internalID) const override;
    status_t HandleMessage(int32 message, const void *data, size_t size) override;

    // BBufferConsumer
    status_t AcceptFormat(const media_destination &dest, media_format *format) override;
    status_t GetNextInput(int32 *cookie, media_input *input) override;
    void DisposeInputCookie(int32 cookie) override;
    void BufferReceived(BBuffer *buffer) override;
    void ProducerDataStatus(const media_destination &dest, int32 status,
                            bigtime_t atPerformanceTime) override;
    status_t GetLatencyFor(const media_destination &dest, bigtime_t *latency,
                           media_node_id *timeSource) override;
    status_t Connected(const media_source &producer, const media_destination &where,
                       const media_format &format, media_input *input) override;
    void Disconnected(const media_source &producer, const media_destination &where) override;
    status_t FormatChanged(const media_source &producer, const media_destination &consumer,
                           int32 changeTag, const media_format &format) override;

    // BBufferProducer
    status_t FormatSuggestionRequested(media_type type, int32 quality,
                                       media_format *format) override;
    status_t FormatProposal(const media_source &output, media_format *format) override;
    status_t FormatChangeRequested(const media_source &source, const media_destination &dest,
                                   media_format *format, int32 *deprecated) override;
    status_t GetNextOutput(int32 *cookie, media_output *output) override;
    status_t DisposeOutputCookie(int32 cookie) override;
    status_t SetBufferGroup(const media_source &source, BBufferGroup *group) override;
    status_t GetLatency(bigtime_t *latency) override;
    status_t PrepareToConnect(const media_source &what, const media_destination &where,
                              media_format *format, media_source *source, char *name) override;
    void Connect(status_t error, const media_source &source, const media_destination &dest,
                 const media_format &format, char *name) override;
    void Disconnect(const media_source &what, const media_destination &where) override;
    void LateNoticeReceived(const media_source &what, bigtime_t howMuch,
                            bigtime_t performanceTime) override;
    void EnableOutput(const media_source &what, bool enabled, int32 *deprecated) override;
    void LatencyChanged(const media_source &source, const media_destination &dest,
                        bigtime_t newLatency, uint32 flags) override;

    // Used by the backend table.
    float Rate() const
    {
        return fRate;
    }
    // Any thread. The control thread uses fPeriod.
    int32 Period() const
    {
        return fPeriodShared.load(std::memory_order_relaxed);
    }
    // The longest period this node can change to; port buffers hold this.
    int32 MaxPeriod() const
    {
        return fMaxPeriod;
    }
    bigtime_t CurrentEventLatency() const
    {
        return EventLatency();
    }
    Port fPorts[kMaxPorts];
    Jack fJacks[kMaxJacks];
    Input fInputs[kMaxInputs];
    BLocker fInputLock; // the main window's view of fInputs
    std::atomic<int32> fCycles{0};
    // Set by mk_open before the node starts, so before any hook that uses
    // them; a BMessenger to a looper that has quit just fails to send.
    BMessenger fWatcher;
    std::atomic<port_id> fMixerPort{-1}; // the system mixer's control port

protected:
    void NodeRegistered() override;
    void HandleEvent(const media_timed_event *event, bigtime_t lateness,
                     bool realTimeEvent) override;

private:
    OutConn *_FindOutput(const media_source &source, int32 *jackIndex = NULL);
    Input *_FindInput(const media_destination &dest);
    void _UpdateLatency();
    void _RunCycle(bigtime_t performanceTime, bigtime_t lateness);
    void _FillInputs(bigtime_t performanceTime);
    void _DiagCapture(int32 index, bigtime_t start, uint32 frames);
    void _DiagForgetCapture(int32 index);
    void _SendOutputs(bigtime_t performanceTime);
    void _QueuePeriodCheck();
    void _FollowMixer(const FollowMixer &m);
    status_t _ChangePeriod(int32 period);
    bool _OthersTakeLength(const OutConn *except, port_id consumerPort) const;
    status_t _MakeOwnGroup(OutConn &o);

    float fRate;
    int32 fPeriod; // control thread
    std::atomic<int32> fPeriodShared;
    const int32 fMaxPeriod;
    bigtime_t fInternalLatency;
    bigtime_t fStartTime = 0;
    uint64 fFramesSent = 0;
    bool fThreadInitDone = false;
};

// ---- backend state (the engine's single client) ----

JdMediaNode *gNode = NULL;
RosterWatcher *gWatcher = NULL;
JdBackendCallbacks gCb;
void *gArg = NULL;

void notify_xrun()
{
    if (gCb.xrun)
        gCb.xrun(gArg);
}

void print_diag();
void check_mixer_period();

void RosterWatcher::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case B_MEDIA_NODE_CREATED:
        case B_MEDIA_NODE_DELETED:
            if (gCb.ports_changed)
                gCb.ports_changed(gArg);
            break;
        case B_MEDIA_CONNECTION_MADE:
        case B_MEDIA_CONNECTION_BROKEN:
            if (gCb.connections_changed)
                gCb.connections_changed(gArg);
            break;
        case B_MEDIA_SERVER_QUIT:
            if (gCb.shutdown)
                gCb.shutdown(gArg);
            break;
        case MSG_DIAG_REPORT:
            print_diag();
            break;
        case MSG_CHECK_PERIOD:
            check_mixer_period();
            break;
        case MSG_PERIOD_CHANGED: {
            int32 period = message->GetInt32("period", 0);
            int32 old = message->GetInt32("old", 0);
            float rate = gNode ? gNode->Rate() : 0.0f;
            if (message->GetBool("refused", false))
                fprintf(stderr,
                        "JackDAW media: a period of %d frames is outside %d..%d; staying at %d\n",
                        (int)period, (int)kMinPeriod, gNode ? (int)gNode->MaxPeriod() : 0,
                        (int)old);
            else
                fprintf(stderr, "JackDAW media: period %d -> %d frames (%.2f ms)%s\n", (int)old,
                        (int)period, rate > 0 ? 1000.0 * period / rate : 0.0,
                        message->GetInt32("noGroup", 0) ? "; an output had no buffers left" : "");
            break;
        }
        default:
            BLooper::MessageReceived(message);
            break;
    }
}

// ---- JdMediaNode ----

JdMediaNode::JdMediaNode(float rate, int32 period)
    : BMediaNode("JackDAW"), BBufferConsumer(B_MEDIA_RAW_AUDIO), BBufferProducer(B_MEDIA_RAW_AUDIO),
      BMediaEventLooper(), fInputLock("JackDAW inputs"), fRate(rate), fPeriod(period),
      fPeriodShared(period), fMaxPeriod(period > JD_MAX_PERIOD ? period : JD_MAX_PERIOD)
{
    fInternalLatency = (bigtime_t)(1000000.0 * period / rate);
    for (int32 j = 0; j < kMaxJacks; j++) {
        for (int32 c = 0; c < kMaxOutConns; c++) {
            OutConn &o = fJacks[j].conns[c];
            o.output.source.port = 0; // set in NodeRegistered
            o.output.source.id = j * kMaxOutConns + c;
        }
    }
    for (int32 i = 0; i < kMaxInputs; i++)
        fInputs[i].input.destination.id = i;
}

JdMediaNode::~JdMediaNode()
{
    Quit(); // waits for the control thread: no cycle runs after this
    for (int32 p = 0; p < kMaxPorts; p++) {
        free(fPorts[p].audio);
        free(fPorts[p].midi);
    }
    for (int32 j = 0; j < kMaxJacks; j++)
        for (int32 c = 0; c < kMaxOutConns; c++)
            delete fJacks[j].conns[c].group;
    for (int32 i = 0; i < kMaxInputs; i++) {
        free(fInputs[i].fifo);
        free(fInputs[i].scratch);
    }
}

BMediaAddOn *JdMediaNode::AddOn(int32 *internalID) const
{
    if (internalID)
        *internalID = 0;
    return NULL; // an application node, not from an add-on
}

status_t JdMediaNode::HandleMessage(int32 message, const void *data, size_t size)
{
    // The event looper's base classes handle every Media Kit message; the
    // only one of our own is the watcher's report of the mixer's period.
    if (message == kFollowMixerCode && size == sizeof(FollowMixer)) {
        _FollowMixer(*(const FollowMixer *)data);
        return B_OK;
    }
    return B_ERROR;
}

void JdMediaNode::NodeRegistered()
{
    for (int32 j = 0; j < kMaxJacks; j++) {
        for (int32 c = 0; c < kMaxOutConns; c++) {
            media_output &out = fJacks[j].conns[c].output;
            out.node = Node();
            out.source.port = ControlPort();
            out.destination = media_destination::null;
        }
    }
    for (int32 i = 0; i < kMaxInputs; i++) {
        media_input &in = fInputs[i].input;
        in.node = Node();
        in.destination.port = ControlPort();
        in.source = media_source::null;
        in.format.type = B_MEDIA_RAW_AUDIO;
        in.format.u.raw_audio = media_raw_audio_format::wildcard;
        in.format.u.raw_audio.frame_rate = fRate;
        snprintf(in.name, sizeof in.name, "In %" B_PRId32, i + 1);
    }
    SetBufferDuration((bigtime_t)(1000000.0 * fPeriod / fRate));
    SetEventLatency(fInternalLatency);
    // Priority stays at the looper default, B_URGENT_PRIORITY: below the
    // system mixer and multi_audio (B_REAL_TIME_PRIORITY), so a heavy plug-in
    // load never starves the system's own audio path.
    Run();
}

OutConn *JdMediaNode::_FindOutput(const media_source &source, int32 *jackIndex)
{
    if (source.port != ControlPort() || source.id < 0 || source.id >= kMaxJacks * kMaxOutConns)
        return NULL;
    int32 j = source.id / kMaxOutConns;
    if (jackIndex)
        *jackIndex = j;
    return &fJacks[j].conns[source.id % kMaxOutConns];
}

Input *JdMediaNode::_FindInput(const media_destination &dest)
{
    if (dest.port != ControlPort() || dest.id < 0 || dest.id >= kMaxInputs)
        return NULL;
    return &fInputs[dest.id];
}

void JdMediaNode::_UpdateLatency()
{
    bigtime_t downstream = 0;
    for (int32 j = 0; j < kMaxJacks; j++)
        for (int32 c = 0; c < kMaxOutConns; c++)
            if (fJacks[j].conns[c].connected && fJacks[j].conns[c].latency > downstream)
                downstream = fJacks[j].conns[c].latency;
    SetEventLatency(downstream + fInternalLatency);
    // Every output is sent this far ahead of play, so each holds that much
    // in buffers at its consumer: a direct hardware input with little
    // latency of its own still waits for the slowest path (the mixer's).
    // Re-make a group of ours that no longer covers it, before it runs dry.
    for (int32 j = 0; j < kMaxJacks; j++) {
        for (int32 c = 0; c < kMaxOutConns; c++) {
            OutConn &o = fJacks[j].conns[c];
            if (o.connected && o.ownGroup && EventLatency() > o.groupLead + BufferDuration() &&
                _MakeOwnGroup(o) != B_OK)
                o.connected = false; // nothing to send from; it stays reserved
        }
    }
}

// ---- consumer side ----

status_t JdMediaNode::AcceptFormat(const media_destination &dest, media_format *format)
{
    Input *in = _FindInput(dest);
    if (!in || in->connected)
        return B_MEDIA_BAD_DESTINATION;
    if (format->type == B_MEDIA_UNKNOWN_TYPE)
        format->type = B_MEDIA_RAW_AUDIO;
    if (format->type != B_MEDIA_RAW_AUDIO)
        return B_MEDIA_BAD_FORMAT;
    media_raw_audio_format &a = format->u.raw_audio;
    const media_raw_audio_format &w = media_raw_audio_format::wildcard;
    if (a.frame_rate == w.frame_rate)
        a.frame_rate = fRate;
    if (fabsf(a.frame_rate - fRate) > 0.5f)
        return B_MEDIA_BAD_FORMAT;
    if (a.channel_count == w.channel_count)
        a.channel_count = 2;
    if (a.channel_count < 1 || a.channel_count > (uint32)kMaxInChannels)
        return B_MEDIA_BAD_FORMAT;
    if (a.format == w.format)
        a.format = media_raw_audio_format::B_AUDIO_FLOAT;
    if (!format_is_convertible(a.format))
        return B_MEDIA_BAD_FORMAT;
    if (a.byte_order == w.byte_order)
        a.byte_order = B_MEDIA_HOST_ENDIAN;
    if (a.byte_order != B_MEDIA_HOST_ENDIAN)
        return B_MEDIA_BAD_FORMAT;
    if (a.buffer_size == w.buffer_size)
        a.buffer_size = fPeriod * a.channel_count * sample_size(a.format);
    return B_OK;
}

status_t JdMediaNode::GetNextInput(int32 *cookie, media_input *input)
{
    // Every connected input, then one free input.
    int32 seen = 0;
    bool freeListed = false;
    for (int32 i = 0; i < kMaxInputs; i++) {
        bool list = fInputs[i].connected || !freeListed;
        if (!list)
            continue;
        if (!fInputs[i].connected)
            freeListed = true;
        if (seen++ == *cookie) {
            *input = fInputs[i].input;
            *cookie += 1;
            return B_OK;
        }
    }
    return B_BAD_INDEX;
}

void JdMediaNode::DisposeInputCookie(int32 /*cookie*/)
{
}

status_t JdMediaNode::Connected(const media_source &producer, const media_destination &where,
                                const media_format &format, media_input *input)
{
    Input *in = _FindInput(where);
    if (!in || in->connected)
        return B_MEDIA_BAD_DESTINATION;
    int32 ch = (int32)format.u.raw_audio.channel_count;
    if (ch < 1 || ch > kMaxInChannels)
        return B_MEDIA_BAD_FORMAT;

    // Sized for the longest period, so a period change needs neither.
    uint32 cap = next_pow2((uint32)fMaxPeriod * kFifoPeriods);
    float *fifo = (float *)calloc((size_t)cap * ch, sizeof(float));
    float *scratch = (float *)calloc((size_t)fMaxPeriod * ch, sizeof(float));
    if (!fifo || !scratch) {
        free(fifo);
        free(scratch);
        return B_NO_MEMORY;
    }

    BAutolock lock(fInputLock);
    free(in->fifo);
    free(in->scratch);
    in->fifo = fifo;
    in->scratch = scratch;
    in->capFrames = cap;
    in->channels = ch;
    in->written = in->read = 0;
    in->primed = false;
    in->primeHalves = kPrimeHalvesFirst;
    in->lastEnd = 0;
    in->owed = 0;
    in->latCur = in->latPrev = -1;
    in->latCycles = 0;
    in->latencyFrames.store(0);
    in->input.source = producer;
    in->input.format = format;
    in->generation++;
    in->connected = true;
    _DiagForgetCapture(int32(in - fInputs));
    *input = in->input;
    return B_OK;
}

void JdMediaNode::Disconnected(const media_source &producer, const media_destination &where)
{
    Input *in = _FindInput(where);
    if (!in || !in->connected || in->input.source != producer)
        return;
    BAutolock lock(fInputLock);
    in->connected = false;
    in->generation++; // routes to it go silent
    in->madeByUs = false;
    _DiagForgetCapture(int32(in - fInputs));
    in->input.source = media_source::null;
    in->input.format.u.raw_audio = media_raw_audio_format::wildcard;
    in->input.format.u.raw_audio.frame_rate = fRate;
    // fifo/scratch stay allocated until reuse or close; nothing reads them now.
}

void JdMediaNode::BufferReceived(BBuffer *buffer)
{
    const media_header *h = buffer->Header();
    media_destination dest(ControlPort(), h->destination);
    Input *in = _FindInput(dest);
    if (!in || !in->connected || h->type != B_MEDIA_RAW_AUDIO) {
        buffer->Recycle();
        return;
    }
    const media_raw_audio_format &a = in->input.format.u.raw_audio;
    size_t ss = sample_size(a.format);
    size_t frameBytes = ss * in->channels;
    uint32 frames = frameBytes ? (uint32)(h->size_used / frameBytes) : 0;
    if (frames > in->capFrames)
        frames = in->capFrames;

    // Overflow (a faster clock, or the cycle not running): drop the oldest.
    uint64 fill = in->written - in->read;
    if (fill + frames > in->capFrames)
        in->read = in->written + frames - in->capFrames;

    const char *src = (const char *)buffer->Data();
    uint32 mask = in->capFrames - 1;
    for (uint32 f = 0; f < frames; f++) {
        float *dst = in->fifo + ((in->written + f) & mask) * in->channels;
        for (int32 c = 0; c < in->channels; c++)
            dst[c] = read_sample(src + (f * in->channels + c) * ss, a.format);
    }
    in->written += frames;
    // When the buffer was captured, in our time source's performance time.
    // A producer on our time source (the same device) stamps it exactly; any
    // other node's stamps count from its own time source, so for those the
    // arrival time on our clock stands in (late by its delivery time).
    if (h->time_source == TimeSource()->ID())
        in->lastEnd = h->start_time + (bigtime_t)(1000000.0 * frames / fRate);
    else
        in->lastEnd = TimeSource()->Now();
    if (gDiagOn && h->time_source == TimeSource()->ID())
        _DiagCapture(int32(in - fInputs), h->start_time, frames);
    buffer->Recycle();
}

/* JACKDAW_DIAG: capture stamps against the frames they carry. Control thread. */
void JdMediaNode::_DiagCapture(int32 index, bigtime_t start, uint32 frames)
{
    int32 measured = gDiag.capInput.load(std::memory_order_relaxed);
    if (measured < 0) {
        gDiag.capInput.store(index, std::memory_order_relaxed);
        gDiag.capCycles0.store(fCycles.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        gDiag.capFirst = gDiag.capPrev = start;
        gDiag.capFrames.store(0, std::memory_order_relaxed);
        gDiag.capSpanUs.store(0, std::memory_order_relaxed);
        gDiag.capBufs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (measured != index)
        return;
    bigtime_t step = start - gDiag.capPrev;
    bigtime_t dur = (bigtime_t)(1000000.0 * frames / fRate);
    if (step * 2 > dur * 3)
        gDiag.capGaps.fetch_add(1, std::memory_order_relaxed);
    if (step > gDiag.capMaxStepUs.load(std::memory_order_relaxed))
        gDiag.capMaxStepUs.store((int32)step, std::memory_order_relaxed);
    // Frames counted up to this buffer's start, so they match the span.
    gDiag.capFrames.fetch_add(frames, std::memory_order_relaxed);
    gDiag.capSpanUs.store(start - gDiag.capFirst, std::memory_order_relaxed);
    gDiag.capPrev = start;
    gDiag.capBufs.fetch_add(1, std::memory_order_relaxed);
}

/* JACKDAW_DIAG: the measured input's connection ended or was remade, so its
 * stamps start over; measure afresh from the next buffer to arrive. */
void JdMediaNode::_DiagForgetCapture(int32 index)
{
    if (gDiag.capInput.load(std::memory_order_relaxed) == index)
        gDiag.capInput.store(-1, std::memory_order_relaxed);
}

void JdMediaNode::ProducerDataStatus(const media_destination & /*dest*/, int32 /*status*/,
                                     bigtime_t /*atPerformanceTime*/)
{
}

status_t JdMediaNode::GetLatencyFor(const media_destination &dest, bigtime_t *latency,
                                    media_node_id *timeSource)
{
    if (!_FindInput(dest))
        return B_MEDIA_BAD_DESTINATION;
    *latency = EventLatency();
    *timeSource = TimeSource()->ID();
    return B_OK;
}

status_t JdMediaNode::FormatChanged(const media_source &producer, const media_destination &consumer,
                                    int32 /*changeTag*/, const media_format &format)
{
    // A producer's new buffer length (multi_audio's "Buffer size" setting)
    // is fine: frames go through the FIFO whatever the buffer length, as long
    // as a buffer fits in half of it. Anything else would need new FIFOs.
    Input *in = _FindInput(consumer);
    if (!in || !in->connected || in->input.source != producer || format.type != B_MEDIA_RAW_AUDIO)
        return B_MEDIA_BAD_FORMAT;
    const media_raw_audio_format &now = in->input.format.u.raw_audio;
    const media_raw_audio_format &a = format.u.raw_audio;
    size_t frameBytes = sample_size(a.format) * a.channel_count;
    if (fabsf(a.frame_rate - now.frame_rate) > 0.5f || a.channel_count != now.channel_count ||
        a.format != now.format || a.byte_order != now.byte_order || frameBytes == 0 ||
        a.buffer_size / frameBytes > in->capFrames / 2)
        return B_MEDIA_BAD_FORMAT;
    bool newLength = a.buffer_size != now.buffer_size;
    {
        BAutolock lock(fInputLock);
        in->input.format = format;
    }
    // The device's buffer length changed, so the mixer's may have too.
    if (newLength)
        _QueuePeriodCheck();
    return B_OK;
}

// ---- producer side ----

status_t JdMediaNode::FormatSuggestionRequested(media_type type, int32 /*quality*/,
                                                media_format *format)
{
    if (type != B_MEDIA_UNKNOWN_TYPE && type != B_MEDIA_RAW_AUDIO)
        return B_MEDIA_BAD_FORMAT;
    format->type = B_MEDIA_RAW_AUDIO;
    format->u.raw_audio = media_raw_audio_format::wildcard;
    format->u.raw_audio.frame_rate = fRate;
    format->u.raw_audio.channel_count = 2;
    format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format->u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
    format->u.raw_audio.buffer_size = fPeriod * 2 * sizeof(float);
    return B_OK;
}

status_t JdMediaNode::FormatProposal(const media_source &output, media_format *format)
{
    int32 j;
    OutConn *o = _FindOutput(output, &j);
    if (!o || !fJacks[j].active.load())
        return B_MEDIA_BAD_SOURCE;
    if (format->type == B_MEDIA_UNKNOWN_TYPE)
        format->type = B_MEDIA_RAW_AUDIO;
    if (format->type != B_MEDIA_RAW_AUDIO)
        return B_MEDIA_BAD_FORMAT;
    // Rate and channel count are fixed; sample format and buffer size are left
    // as wildcards for the consumer to choose (multi_audio wants its device's
    // own format and length), and PrepareToConnect checks what came back.
    media_raw_audio_format &a = format->u.raw_audio;
    a = media_raw_audio_format::wildcard;
    a.frame_rate = fRate;
    a.channel_count = fJacks[j].channels;
    a.byte_order = B_MEDIA_HOST_ENDIAN;
    return B_OK;
}

/* A consumer asks for another format: multi_audio does, for its new buffer
 * length, when JackDAW feeds a hardware input directly. Only the length may
 * change, and it becomes the period of the whole node, so it is refused when
 * another output goes to a consumer that would still want the old one. */
status_t JdMediaNode::FormatChangeRequested(const media_source &source,
                                            const media_destination &dest,
                                            media_format *format, int32 * /*deprecated*/)
{
    int32 j;
    OutConn *o = _FindOutput(source, &j);
    if (!o || !o->connected || o->output.destination != dest)
        return B_MEDIA_BAD_SOURCE;
    if (format->type == B_MEDIA_UNKNOWN_TYPE)
        format->type = B_MEDIA_RAW_AUDIO;
    if (format->type != B_MEDIA_RAW_AUDIO)
        return B_MEDIA_BAD_FORMAT;
    media_raw_audio_format &a = format->u.raw_audio;
    const media_raw_audio_format &now = o->output.format.u.raw_audio;
    const media_raw_audio_format &w = media_raw_audio_format::wildcard;
    if (a.frame_rate == w.frame_rate)
        a.frame_rate = now.frame_rate;
    if (a.channel_count == w.channel_count)
        a.channel_count = now.channel_count;
    if (a.format == w.format)
        a.format = now.format;
    if (a.byte_order == w.byte_order)
        a.byte_order = now.byte_order;
    if (a.buffer_size == w.buffer_size)
        a.buffer_size = now.buffer_size;
    size_t frame = sample_size(a.format) * a.channel_count;
    if (fabsf(a.frame_rate - fRate) > 0.5f || a.channel_count != now.channel_count ||
        a.format != now.format || a.byte_order != now.byte_order || frame == 0 ||
        a.buffer_size % frame != 0)
        return B_MEDIA_BAD_FORMAT;
    // A connection with an open length carries the longest we may send,
    // and keeps it: only a consumer that chose its length sets the period.
    if (!o->fixedLength)
        return a.buffer_size == now.buffer_size ? B_OK : B_MEDIA_BAD_FORMAT;
    int32 period = (int32)(a.buffer_size / frame);
    if (period == fPeriod)
        return B_OK;

    if (!_OthersTakeLength(o, dest.port))
        return B_MEDIA_BAD_FORMAT;
    return _ChangePeriod(period);
}

/* Whether every output but `except` could take a new period: it goes to the
 * system mixer (any length) or to `consumerPort`, the node asking for it. */
bool JdMediaNode::_OthersTakeLength(const OutConn *except, port_id consumerPort) const
{
    port_id mixer = fMixerPort.load(std::memory_order_relaxed);
    for (int32 k = 0; k < kMaxJacks; k++) {
        for (int32 c = 0; c < kMaxOutConns; c++) {
            const OutConn &other = fJacks[k].conns[c];
            if (!other.reserved || &other == except)
                continue;
            port_id port = other.output.destination.port;
            if (port != consumerPort && port != mixer)
                return false;
        }
    }
    return true;
}

status_t JdMediaNode::GetNextOutput(int32 *cookie, media_output *output)
{
    // Every connected output of every active jack, then one free output per
    // jack (so each jack can always be connected once more).
    int32 seen = 0;
    for (int32 pass = 0; pass < 2; pass++) {
        for (int32 j = 0; j < kMaxJacks; j++) {
            Jack &jk = fJacks[j];
            if (!jk.active.load())
                continue;
            for (int32 c = 0; c < kMaxOutConns; c++) {
                OutConn &o = jk.conns[c];
                bool want = (pass == 0) ? o.reserved : !o.reserved;
                if (!want)
                    continue;
                if (seen++ == *cookie) {
                    *output = o.output;
                    if (!o.reserved) {
                        output->format.type = B_MEDIA_RAW_AUDIO;
                        output->format.u.raw_audio = media_raw_audio_format::wildcard;
                        output->format.u.raw_audio.frame_rate = fRate;
                        output->format.u.raw_audio.channel_count = jk.channels;
                        snprintf(output->name, sizeof output->name, "%s", jk.name);
                    }
                    *cookie += 1;
                    return B_OK;
                }
                if (pass == 1)
                    break; // one free output per jack
            }
        }
    }
    return B_BAD_INDEX;
}

status_t JdMediaNode::DisposeOutputCookie(int32 /*cookie*/)
{
    return B_OK;
}

status_t JdMediaNode::SetBufferGroup(const media_source &source, BBufferGroup *group)
{
    OutConn *o = _FindOutput(source);
    if (!o || !o->reserved)
        return B_MEDIA_BAD_SOURCE;
    // NULL asks for our own group, so only a consumer's group already in use
    // is a no-op.
    if (group != NULL && group == o->group)
        return B_OK;
    delete o->group;
    o->group = group;
    o->ownGroup = false;
    if (!o->group)
        return _MakeOwnGroup(*o);
    return B_OK;
}

/* A group of our own for o's buffers: enough for every buffer between being
 * sent, the node's event latency (or o's own path, if that is longer and not
 * counted in yet) ahead of play, and coming back. */
status_t JdMediaNode::_MakeOwnGroup(OutConn &o)
{
    delete o.group;
    o.group = NULL;
    o.ownGroup = false;
    bigtime_t lead = o.latency + fInternalLatency;
    if (EventLatency() > lead)
        lead = EventLatency();
    o.groupLead = lead;
    int32 count = (int32)((lead + BufferDuration() - 1) / BufferDuration()) + 3;
    o.group = new (std::nothrow) BBufferGroup(o.output.format.u.raw_audio.buffer_size, count);
    if (!o.group || o.group->InitCheck() != B_OK) {
        delete o.group;
        o.group = NULL;
        return B_NO_MEMORY;
    }
    o.ownGroup = true;
    return B_OK;
}

status_t JdMediaNode::GetLatency(bigtime_t *latency)
{
    *latency = EventLatency() + SchedulingLatency();
    return B_OK;
}

status_t JdMediaNode::PrepareToConnect(const media_source &what, const media_destination &where,
                                       media_format *format, media_source *source, char *name)
{
    int32 j;
    OutConn *o = _FindOutput(what, &j);
    if (!o || !fJacks[j].active.load())
        return B_MEDIA_BAD_SOURCE;
    if (o->reserved)
        return B_MEDIA_ALREADY_CONNECTED;
    if (format->type != B_MEDIA_UNKNOWN_TYPE && format->type != B_MEDIA_RAW_AUDIO)
        return B_MEDIA_BAD_FORMAT;
    format->type = B_MEDIA_RAW_AUDIO;
    media_raw_audio_format &a = format->u.raw_audio;
    const media_raw_audio_format &w = media_raw_audio_format::wildcard;
    if (a.frame_rate == w.frame_rate)
        a.frame_rate = fRate;
    if (a.channel_count == w.channel_count)
        a.channel_count = fJacks[j].channels;
    if (a.format == w.format)
        a.format = media_raw_audio_format::B_AUDIO_FLOAT;
    if (a.byte_order == w.byte_order)
        a.byte_order = B_MEDIA_HOST_ENDIAN;
    size_t frame = (size_t)a.channel_count * sample_size(a.format);
    size_t want = (size_t)fPeriod * frame;
    // A consumer that leaves the length open gets the longest we may send:
    // it sizes its buffering for that, and the period may grow up to it.
    bool fixedLength = a.buffer_size != w.buffer_size;
    if (!fixedLength)
        a.buffer_size = want = (size_t)fMaxPeriod * frame;
    // Our clock is the hardware's: refuse another rate or buffer length rather
    // than resample.
    if (fabsf(a.frame_rate - fRate) > 0.5f || a.channel_count != (uint32)fJacks[j].channels ||
        !format_is_convertible(a.format) || a.byte_order != B_MEDIA_HOST_ENDIAN || frame == 0 ||
        a.buffer_size != want)
        return B_MEDIA_BAD_FORMAT;
    // Buffers are stamped in our time source's performance time, and a
    // consumer plays them by its own. One on another clock (a second sound
    // card, while the system mixer and its time source are on the first)
    // drifts from ours; in testing, a UMC204HD that was not the default
    // output also never gave our buffers back, so the output ran out of
    // them at once. Two clocks would need resampling; refuse instead.
    bigtime_t consumerLatency;
    media_node_id consumerClock;
    if (FindLatencyFor(where, &consumerLatency, &consumerClock) != B_OK ||
        consumerClock != TimeSource()->ID())
        return B_MEDIA_BAD_DESTINATION;

    o->reserved = true;
    o->fixedLength = fixedLength;
    o->output.destination = where;
    o->output.format = *format;
    snprintf(o->output.name, sizeof o->output.name, "%s", fJacks[j].name);
    *source = o->output.source;
    strlcpy(name, o->output.name, B_MEDIA_NAME_LENGTH);
    return B_OK;
}

void JdMediaNode::Connect(status_t error, const media_source &source, const media_destination &dest,
                          const media_format &format, char *name)
{
    int32 j;
    OutConn *o = _FindOutput(source, &j);
    if (!o)
        return;
    if (error != B_OK) {
        o->reserved = false;
        o->output.destination = media_destination::null;
        return;
    }
    o->output.destination = dest;
    o->output.format = format;
    strlcpy(name, o->output.name, B_MEDIA_NAME_LENGTH);

    media_node_id ts;
    if (FindLatencyFor(dest, &o->latency, &ts) != B_OK)
        o->latency = 0;
    if (!o->group)
        SetBufferGroup(source, NULL);
    o->enabled = true;
    o->connected = (o->group != NULL);
    _UpdateLatency();
}

void JdMediaNode::Disconnect(const media_source &what, const media_destination &where)
{
    OutConn *o = _FindOutput(what);
    if (!o || !o->reserved || o->output.destination != where)
        return;
    o->connected = false;
    o->reserved = false;
    o->fixedLength = false;
    o->output.destination = media_destination::null;
    delete o->group;
    o->group = NULL;
    o->ownGroup = false;
    o->latency = 0;
    _UpdateLatency();
    // That may have been what held the period off the mixer's.
    _QueuePeriodCheck();
}

void JdMediaNode::LateNoticeReceived(const media_source &what, bigtime_t howMuch,
                                     bigtime_t /*performanceTime*/)
{
    if (!_FindOutput(what))
        return;
    gDiag.lateNotes.fetch_add(1, std::memory_order_relaxed);
    if (howMuch > gDiag.lateUs.load(std::memory_order_relaxed))
        gDiag.lateUs.store((int32)howMuch, std::memory_order_relaxed);
    notify_xrun();
    // A consumer got our buffer late: produce earlier from now on (bounded),
    // as SoundPlayNode does.
    fInternalLatency += howMuch;
    bigtime_t cap = 4 * BufferDuration() + 10000;
    if (fInternalLatency > cap)
        fInternalLatency = cap;
    _UpdateLatency();
}

void JdMediaNode::EnableOutput(const media_source &what, bool enabled, int32 * /*deprecated*/)
{
    OutConn *o = _FindOutput(what);
    if (o)
        o->enabled = enabled;
}

void JdMediaNode::LatencyChanged(const media_source &source, const media_destination &dest,
                                 bigtime_t newLatency, uint32 /*flags*/)
{
    OutConn *o = _FindOutput(source);
    if (!o || o->output.destination != dest)
        return;
    o->latency = newLatency;
    _UpdateLatency();
    // The system mixer sends this when its output format changes, which is
    // the only notice there is of a new hardware buffer length.
    _QueuePeriodCheck();
}

/* Ask the roster watcher to read the system mixer's period (it may not query
 * the mixer from here: the mixer could be waiting on us). One at a time. */
void JdMediaNode::_QueuePeriodCheck()
{
    if (gCheckQueued.exchange(1, std::memory_order_acq_rel) != 0)
        return;
    BMessage check(MSG_CHECK_PERIOD);
    if (fWatcher.SendMessage(&check, (BHandler *)NULL, 0) != B_OK)
        gCheckQueued.store(0, std::memory_order_release);
}

/* The watcher's reading of the system mixer's output. Follow its period when
 * every output goes to the mixer; a hardware input connected directly asks
 * for its own length instead (FormatChangeRequested). */
void JdMediaNode::_FollowMixer(const FollowMixer &m)
{
    fMixerPort.store(m.mixerPort, std::memory_order_relaxed);
    if (m.period == fPeriod || fabsf(m.rate - fRate) > 0.5f)
        return; // a new rate needs a restart of JackDAW
    for (int32 j = 0; j < kMaxJacks; j++)
        for (int32 c = 0; c < kMaxOutConns; c++)
            if (fJacks[j].conns[c].connected &&
                fJacks[j].conns[c].output.destination.port != m.mixerPort)
                return;
    _ChangePeriod(m.period);
}

/* Run at `period` frames from the next cycle on. Control thread, between
 * cycles. */
status_t JdMediaNode::_ChangePeriod(int32 period)
{
    int32 old = fPeriod;
    if (period == old)
        return B_OK;
    if (period < kMinPeriod || period > fMaxPeriod) {
        BMessage note(MSG_PERIOD_CHANGED);
        note.AddInt32("period", period);
        note.AddInt32("old", old);
        note.AddBool("refused", true);
        fWatcher.SendMessage(&note, (BHandler *)NULL, 0);
        return B_MEDIA_BAD_FORMAT;
    }

    // The engine's buffers already hold the longest period, so this only
    // sets its cycle length.
    if (gCb.buffer_size)
        gCb.buffer_size((jd_nframes_t)period, gArg);
    fPeriod = period;
    fPeriodShared.store(period, std::memory_order_relaxed);
    bigtime_t duration = (bigtime_t)(1000000.0 * period / fRate);
    SetBufferDuration(duration);
    fInternalLatency = duration; // late notices start over

    int32 noGroup = 0;
    for (int32 j = 0; j < kMaxJacks; j++) {
        for (int32 c = 0; c < kMaxOutConns; c++) {
            OutConn &o = fJacks[j].conns[c];
            if (!o.reserved)
                continue;
            // A fixed length is the consumer's, which asked for this one; an
            // open one already allows every period up to fMaxPeriod.
            media_raw_audio_format &a = o.output.format.u.raw_audio;
            if (o.fixedLength)
                a.buffer_size = (size_t)period * a.channel_count * sample_size(a.format);
            if (!o.connected)
                continue;
            media_node_id ts;
            if (FindLatencyFor(o.output.destination, &o.latency, &ts) != B_OK)
                o.latency = 0;
            // Our groups hold buffers of the old length and count: make them
            // again in _UpdateLatency. A consumer's own group stays: a buffer
            // of at least the new length is all a request needs.
            if (o.ownGroup)
                o.groupLead = -BufferDuration() - 1;
        }
    }
    _UpdateLatency();
    for (int32 j = 0; j < kMaxJacks; j++)
        for (int32 c = 0; c < kMaxOutConns; c++)
            if (fJacks[j].conns[c].reserved && !fJacks[j].conns[c].connected)
                noGroup++;

    // The device restarted its streams: read every input afresh.
    for (int32 i = 0; i < kMaxInputs; i++) {
        Input &in = fInputs[i];
        if (!in.connected)
            continue;
        in.read = in.written;
        in.primed = false;
        in.primeHalves = kPrimeHalvesFirst;
        in.owed = 0;
        in.latCur = in.latPrev = -1;
        in.latCycles = 0;
        _DiagForgetCapture(i);
    }

    BMessage note(MSG_PERIOD_CHANGED);
    note.AddInt32("period", period);
    note.AddInt32("old", old);
    note.AddInt32("noGroup", noGroup);
    fWatcher.SendMessage(&note, (BHandler *)NULL, 0);
    return B_OK;
}

// ---- the cycle ----

void JdMediaNode::HandleEvent(const media_timed_event *event, bigtime_t lateness,
                              bool /*realTimeEvent*/)
{
    switch (event->type) {
        case BTimedEventQueue::B_START:
            if (RunState() != B_STARTED) {
                fFramesSent = 0;
                fStartTime = event->event_time;
                media_timed_event first(event->event_time, SEND_CYCLE_EVENT);
                EventQueue()->AddEvent(first);
            }
            break;
        case BTimedEventQueue::B_STOP:
            EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true, SEND_CYCLE_EVENT);
            break;
        case SEND_CYCLE_EVENT:
            if (RunState() == B_STARTED)
                _RunCycle(event->event_time, lateness);
            break;
        default:
            break;
    }
}

void JdMediaNode::_RunCycle(bigtime_t performanceTime, bigtime_t lateness)
{
    if (!fThreadInitDone) {
        fThreadInitDone = true;
        if (gCb.thread_init)
            gCb.thread_init(gArg);
    }
    // Woken a whole period late: the cycle cannot make its deadline.
    if (lateness > BufferDuration()) {
        gDiag.lateWake.fetch_add(1, std::memory_order_relaxed);
        notify_xrun();
    }

    _FillInputs(performanceTime);
    if (gCb.process)
        gCb.process((jd_nframes_t)fPeriod, gArg);
    _SendOutputs(performanceTime);
    fCycles.fetch_add(1, std::memory_order_release);

    fFramesSent += (uint64)fPeriod;
    bigtime_t next = fStartTime + (bigtime_t)(1000000.0 * (double)fFramesSent / fRate);
    media_timed_event nextEvent(next, SEND_CYCLE_EVENT);
    EventQueue()->AddEvent(nextEvent);
}

void JdMediaNode::_FillInputs(bigtime_t performanceTime)
{
    // Take one period from every connected input into its planar scratch.
    for (int32 i = 0; i < kMaxInputs; i++) {
        Input &in = fInputs[i];
        if (!in.connected)
            continue;
        uint64 avail = in.written - in.read;
        // Frames of cycles that ran dry arrive late; drop them, so what
        // follows plays (and records) where it belongs in time.
        if (in.owed > 0) {
            uint64 drop = in.owed < avail ? in.owed : avail;
            in.read += drop;
            in.owed -= drop;
            avail -= drop;
        }
        if (!in.primed && 2 * avail >= (uint64)fPeriod * in.primeHalves) {
            // Start reading at exactly the wait, dropping anything older: a
            // backlog would stay in the FIFO as extra latency for good. One
            // builds up when cycles pause while capture goes on, as they do
            // for a moment after the period shrinks (the next cycle waits
            // for the shorter lead).
            uint64 target = ((uint64)fPeriod * in.primeHalves + 1) / 2;
            if (avail > target) {
                in.read = in.written - target;
                avail = target;
            }
            in.primed = true;
        }
        if (!in.primed || avail < (uint64)fPeriod) {
            if (in.primed) { // ran dry
                if (gDiagOn)
                    gDiag.capDry.fetch_add(1, std::memory_order_relaxed);
                notify_xrun();
                if (gHoldLatency.load(std::memory_order_relaxed) != 0) {
                    // A take is open: keep the latency it was placed by.
                    // This cycle plays silence and its frames are dropped
                    // when they come. An input that has stopped altogether
                    // starts over once it is owed half the FIFO.
                    in.owed += (uint64)fPeriod;
                    if (in.owed > in.capFrames / 2) {
                        in.primed = false;
                        in.owed = 0;
                        in.read = in.written;
                        in.latCur = in.latPrev = -1;
                    }
                } else {
                    // Wait for it to refill, with more margin. That moves
                    // the latency, which nothing is holding now.
                    in.primed = false;
                    if (in.primeHalves < kPrimeHalvesMax)
                        in.primeHalves++;
                    in.latCur = in.latPrev = -1;
                }
            }
            memset(in.scratch, 0, sizeof(float) * fPeriod * in.channels);
            continue;
        }
        // Capture-to-playback offset of the frames read now: they were
        // captured at lastEnd - avail, and play out with this cycle at
        // performanceTime. The engine shifts a recorded take back by this.
        // A capture stamp can only be late (it comes from when the driver
        // saw the buffer complete), which makes the offset look smaller, so
        // the largest offset over the last window or two is the one
        // reported.
        bigtime_t captured = in.lastEnd - (bigtime_t)(1000000.0 * (double)avail / fRate);
        double offset = (double)(performanceTime - captured) * fRate / 1000000.0;
        int32 frames = offset > 0 ? (int32)(offset + 0.5) : 0;
        if (frames > in.latCur)
            in.latCur = frames;
        if (++in.latCycles * (bigtime_t)fPeriod * 1000000 >= kLatencyWindowUs * (bigtime_t)fRate) {
            in.latPrev = in.latCur;
            in.latCur = frames;
            in.latCycles = 0;
        }
        in.latencyFrames.store(in.latCur > in.latPrev ? in.latCur : in.latPrev,
                               std::memory_order_relaxed);
        if (gDiagOn && i == gDiag.capInput.load(std::memory_order_relaxed)) {
            if (frames < gDiag.capLatMin.load(std::memory_order_relaxed))
                gDiag.capLatMin.store(frames, std::memory_order_relaxed);
            if (frames > gDiag.capLatMax.load(std::memory_order_relaxed))
                gDiag.capLatMax.store(frames, std::memory_order_relaxed);
        }

        uint32 mask = in.capFrames - 1;
        for (int32 f = 0; f < fPeriod; f++) {
            const float *src = in.fifo + ((in.read + f) & mask) * in.channels;
            for (int32 c = 0; c < in.channels; c++)
                in.scratch[c * fPeriod + f] = src[c];
        }
        in.read += (uint64)fPeriod;
    }

    // Then each routed capture port copies its channel.
    for (int32 p = 0; p < kMaxPorts; p++) {
        Port &port = fPorts[p];
        if (!port.used.load(std::memory_order_acquire))
            continue;
        if (port.type == JD_PORT_MIDI) {
            if (port.direction == JD_PORT_INPUT)
                port.midi->count = 0; // MIDI comes with the Midi Kit backend
            continue;
        }
        if (port.direction != JD_PORT_INPUT)
            continue;
        int32 r = port.route.load(std::memory_order_acquire);
        int32 ii = (r >> 8) & 0xff, ch = r & 0xff;
        const Input *in = (r >= 0 && ii < kMaxInputs) ? &fInputs[ii] : NULL;
        if (in && in->connected && in->generation == (uint8)(r >> 16) && ch < in->channels)
            memcpy(port.audio, in->scratch + ch * fPeriod, sizeof(float) * fPeriod);
        else
            memset(port.audio, 0, sizeof(float) * fPeriod);
    }
}

void JdMediaNode::_SendOutputs(bigtime_t performanceTime)
{
    for (int32 j = 0; j < kMaxJacks; j++) {
        Jack &jk = fJacks[j];
        if (!jk.active.load())
            continue;
        const float *chan[2] = {NULL, NULL};
        for (int32 c = 0; c < jk.channels; c++) {
            int32 slot = jk.portSlot[c].load(std::memory_order_acquire);
            if (slot >= 0 && fPorts[slot].used.load(std::memory_order_acquire))
                chan[c] = fPorts[slot].audio;
        }
        for (int32 k = 0; k < kMaxOutConns; k++) {
            OutConn &o = jk.conns[k];
            if (!o.connected || !o.enabled || !o.group)
                continue;
            const media_raw_audio_format &a = o.output.format.u.raw_audio;
            size_t ss = sample_size(a.format);
            size_t bytes = (size_t)fPeriod * jk.channels * ss; // <= a.buffer_size
            BBuffer *buf = o.group->RequestBuffer(bytes, 0); // never wait
            if (!buf) {
                gDiag.noBuffer.fetch_add(1, std::memory_order_relaxed);
                notify_xrun();
                continue;
            }
            char *dst = (char *)buf->Data();
            for (int32 f = 0; f < fPeriod; f++)
                for (int32 c = 0; c < jk.channels; c++, dst += ss)
                    write_sample(dst, a.format, chan[c] ? chan[c][f] : 0.0f);
            media_header *h = buf->Header();
            h->type = B_MEDIA_RAW_AUDIO;
            h->size_used = bytes;
            h->time_source = TimeSource()->ID();
            h->start_time = performanceTime;
            if (gDiagOn) {
                if (j == 0) {
                    float peak = 0.0f;
                    for (int32 c = 0; c < jk.channels; c++)
                        for (int32 f = 0; chan[c] && f < fPeriod; f++)
                            peak = fmaxf(peak, fabsf(chan[c][f]));
                    int32 milli = (int32)(peak * 1000.0f);
                    if (milli > gDiag.peakMilli.load(std::memory_order_relaxed))
                        gDiag.peakMilli.store(milli, std::memory_order_relaxed);
                }
                int32 lead = (int32)(performanceTime - TimeSource()->Now());
                if (lead < gDiag.minLeadUs.load(std::memory_order_relaxed))
                    gDiag.minLeadUs.store(lead, std::memory_order_relaxed);
            }
            if (SendBuffer(buf, o.output.source, o.output.destination) != B_OK) {
                gDiag.sendFail.fetch_add(1, std::memory_order_relaxed);
                buf->Recycle();
            } else {
                gDiag.sent.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ---- jacks for output ports ----

/* Which jack and channel an output port feeds, from its name: out_N -> jack
 * (N-1)/2 channel (N-1)%2 ("Master" for out_1/out_2), "metronome" -> the
 * Metronome jack. Other names have no jack. */
bool jack_for_port(const char *shortName, int32 *jack, int32 *channel)
{
    unsigned n;
    char tail;
    if (sscanf(shortName, "out_%u%c", &n, &tail) == 1 && n >= 1 &&
        (n - 1) / 2 < (unsigned)kMetronomeJack) {
        *jack = (int32)((n - 1) / 2);
        *channel = (int32)((n - 1) % 2);
        return true;
    }
    if (strcmp(shortName, "metronome") == 0) {
        *jack = kMetronomeJack;
        *channel = 0;
        return true;
    }
    return false;
}

void setup_jacks(JdMediaNode *node)
{
    for (int32 j = 0; j < kMaxJacks; j++) {
        Jack &jk = node->fJacks[j];
        if (j == kMetronomeJack) {
            snprintf(jk.name, sizeof jk.name, "Metronome");
            jk.channels = 1;
        } else if (j == 0) {
            snprintf(jk.name, sizeof jk.name, "Master");
            jk.channels = 2;
        } else {
            snprintf(jk.name, sizeof jk.name, "Out %d+%d", (int)(2 * j + 1), (int)(2 * j + 2));
            jk.channels = 2;
        }
    }
}

// ---- capture sources ----

struct CaptureSource {
    char name[192];
    media_node node; // the producer
    media_output output;
    int32 channel;
    int32 input; // >= 0: an existing input of ours (patched in Cortex)
};

/* Hardware capture channels (every audio output of every physical-input
 * node), and the channels of inputs patched into JackDAW from elsewhere.
 * Non-RT. */
void list_capture_sources(std::vector<CaptureSource> &out)
{
    BMediaRoster *roster = BMediaRoster::CurrentRoster();
    if (!roster || !gNode)
        return;
    live_node_info nodes[64];
    int32 count = 64;
    if (roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL, B_PHYSICAL_INPUT) == B_OK) {
        for (int32 n = 0; n < count; n++) {
            if (nodes[n].node.node == gNode->ID())
                continue;
            media_output outs[16];
            int32 nouts = 0;
            if (roster->GetAllOutputsFor(nodes[n].node, outs, 16, &nouts) != B_OK)
                continue;
            for (int32 o = 0; o < nouts; o++) {
                const media_format &f = outs[o].format;
                if (f.type != B_MEDIA_RAW_AUDIO && f.type != B_MEDIA_UNKNOWN_TYPE)
                    continue;
                int32 ch = (int32)f.u.raw_audio.channel_count;
                if (ch < 1 || ch > kMaxInChannels)
                    ch = 2;
                for (int32 c = 0; c < ch; c++) {
                    CaptureSource s;
                    snprintf(s.name, sizeof s.name, "%s%s / %s / %d", kCapturePrefix, nodes[n].name,
                             outs[o].name, (int)(c + 1));
                    s.node = nodes[n].node;
                    s.output = outs[o];
                    s.channel = c;
                    s.input = -1;
                    out.push_back(s);
                }
            }
        }
    }

    // Inputs connected from elsewhere (not one of the outputs listed above).
    BAutolock lock(gNode->fInputLock);
    for (int32 i = 0; i < kMaxInputs; i++) {
        const Input &in = gNode->fInputs[i];
        if (!in.connected)
            continue;
        bool listed = false;
        for (size_t k = 0; k < out.size() && !listed; k++)
            listed = out[k].output.source == in.input.source;
        if (listed)
            continue;
        for (int32 c = 0; c < in.channels; c++) {
            CaptureSource s;
            snprintf(s.name, sizeof s.name, "%sJackDAW %s / %d", kCapturePrefix, in.input.name,
                     (int)(c + 1));
            s.channel = c;
            s.input = i;
            out.push_back(s);
        }
    }
}

int32 find_input_from(const media_source &source)
{
    for (int32 i = 0; i < kMaxInputs; i++)
        if (gNode->fInputs[i].connected && gNode->fInputs[i].input.source == source)
            return i;
    return -1;
}

Port *port_by_full_name(const char *name)
{
    if (!gNode || !name)
        return NULL;
    for (int32 p = 0; p < kMaxPorts; p++) {
        Port &port = gNode->fPorts[p];
        if (port.used.load(std::memory_order_acquire) && strcmp(port.fullName, name) == 0)
            return &port;
    }
    return NULL;
}

/* The format of the system mixer's (first) output: the hardware's rate and
 * buffer length. */
bool mixer_output_format(BMediaRoster *roster, float *rate, int32 *period,
                         port_id *mixerPort = NULL)
{
    media_node mixer;
    if (roster->GetAudioMixer(&mixer) != B_OK)
        return false;
    if (mixerPort)
        *mixerPort = mixer.port;
    media_output out;
    int32 n = 0;
    bool ok = roster->GetConnectedOutputsFor(mixer, &out, 1, &n) == B_OK && n == 1;
    roster->ReleaseNode(mixer);
    if (!ok)
        return false;
    const media_raw_audio_format &a = out.format.u.raw_audio;
    size_t frame = sample_size(a.format) * a.channel_count;
    if (a.frame_rate <= 0 || frame == 0 || a.buffer_size < frame)
        return false;
    *rate = a.frame_rate;
    *period = (int32)(a.buffer_size / frame);
    return true;
}

/* The live node whose control port is `port` (media_node carries it), or -1. */
media_node_id node_for_port(BMediaRoster *roster, port_id port)
{
    live_node_info nodes[64];
    int32 count = 64;
    if (roster->GetLiveNodes(nodes, &count) != B_OK)
        return -1;
    for (int32 i = 0; i < count; i++)
        if (nodes[i].node.port == port)
            return nodes[i].node.node;
    return -1;
}

/* Undo connections from hardware capture outputs to consumers that are gone.
 * A node whose team died (a crash, or a kill) never disconnects, so
 * multi_audio keeps the output reserved for it and refuses every new
 * connection ("Already connected"). A consumer's destination port is its
 * control port, which goes away with the team, so a dead port means a dead
 * consumer. BMediaRoster::Disconnect only needs the producer's side for this
 * (it queries the producer and consumer ports separately, MediaRoster.cpp). */
void release_dead_capture_connections(BMediaRoster *roster)
{
    live_node_info nodes[64];
    int32 count = 64;
    if (roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL, B_PHYSICAL_INPUT) != B_OK)
        return;
    for (int32 n = 0; n < count; n++) {
        media_output outs[16];
        int32 nouts = 0;
        if (roster->GetConnectedOutputsFor(nodes[n].node, outs, 16, &nouts) != B_OK)
            continue;
        for (int32 o = 0; o < nouts; o++) {
            port_info info;
            if (get_port_info(outs[o].destination.port, &info) == B_OK)
                continue; // the consumer is alive
            // The consumer's node id is unknown (the node is gone); any valid
            // id gets the producer side disconnected.
            // The consumer half of this always fails (its port is gone), so
            // the result says nothing; the producer side is what frees it.
            roster->Disconnect(nodes[n].node.node, outs[o].source, nodes[n].node.node,
                               outs[o].destination);
            fprintf(stderr, "JackDAW media: released %s / %s from a consumer that is gone\n",
                    nodes[n].name, outs[o].name);
        }
    }
}

/* Connect one of our jacks' free outputs to a free input of `consumer`. */
status_t connect_jack(BMediaRoster *roster, int32 jack, const media_node &consumer)
{
    media_output outs[kMaxJacks * kMaxOutConns];
    int32 n = 0;
    if (roster->GetFreeOutputsFor(gNode->Node(), outs, kMaxJacks * kMaxOutConns, &n) != B_OK)
        return B_ERROR;
    const media_output *free = NULL;
    for (int32 i = 0; i < n && !free; i++)
        if (outs[i].source.id / kMaxOutConns == jack)
            free = &outs[i];
    if (!free)
        return B_BUSY;
    media_input in;
    int32 nin = 0;
    status_t err = roster->GetFreeInputsFor(consumer, &in, 1, &nin, B_MEDIA_RAW_AUDIO);
    if (err != B_OK || nin < 1)
        return err != B_OK ? err : B_BUSY;
    media_format fmt;
    fmt.type = B_MEDIA_RAW_AUDIO;
    fmt.u.raw_audio = media_raw_audio_format::wildcard;
    fmt.u.raw_audio.frame_rate = gNode->Rate();
    fmt.u.raw_audio.channel_count = gNode->fJacks[jack].channels;
    media_output madeOut;
    media_input madeIn;
    return roster->Connect(free->source, in.destination, &fmt, &madeOut, &madeIn);
}

/* The system mixer's output may have a new buffer length: tell the node,
 * which decides on its own thread whether to follow it. Runs on the watcher
 * (which mk_close stops before the node goes away). */
void check_mixer_period()
{
    gCheckQueued.store(0, std::memory_order_release);
    BMediaRoster *roster = BMediaRoster::CurrentRoster();
    if (!gNode || !roster)
        return;
    FollowMixer m;
    if (!mixer_output_format(roster, &m.rate, &m.period, &m.mixerPort))
        return;
    if (m.period == gNode->Period())
        return;
    write_port_etc(gNode->ControlPort(), kFollowMixerCode, &m, sizeof m, B_RELATIVE_TIMEOUT,
                   500000);
}

/* JACKDAW_DIAG: what the node did in the last 2 s. Runs on the watcher. */
void print_diag()
{
    if (!gNode)
        return;
    int32 lead = gDiag.minLeadUs.exchange(INT32_MAX, std::memory_order_relaxed);
    fprintf(stderr,
            "JackDAW media: cycles %d, sent %d, send failed %d, no buffer %d, late notices %d "
            "(max %d us), late wakes %d, master peak %.3f, least lead %d us, event latency "
            "%lld us\n",
            (int)gNode->fCycles.load(), (int)gDiag.sent.exchange(0),
            (int)gDiag.sendFail.exchange(0), (int)gDiag.noBuffer.exchange(0),
            (int)gDiag.lateNotes.exchange(0), (int)gDiag.lateUs.exchange(0),
            (int)gDiag.lateWake.exchange(0), gDiag.peakMilli.exchange(0) / 1000.0,
            lead == INT32_MAX ? -1 : (int)lead, (long long)gNode->CurrentEventLatency());

    int32 capIn = gDiag.capInput.load(std::memory_order_relaxed);
    if (capIn < 0)
        return;
    // Two rates against the capture's frame count, in parts per million:
    // its own stamps (positive: the stamps run ahead of the frames, i.e.
    // buffers are missing or short), and our cycle (positive: we consume
    // faster than it delivers, so the input runs dry now and then).
    double rate = gNode->Rate();
    double frames = (double)gDiag.capFrames.load(std::memory_order_relaxed);
    double span = (double)gDiag.capSpanUs.load(std::memory_order_relaxed);
    double cycleFrames = (double)(gNode->fCycles.load() -
                                  gDiag.capCycles0.load(std::memory_order_relaxed)) *
                         gNode->Period();
    double stampPpm = span > 0 ? (span - frames * 1e6 / rate) / span * 1e6 : 0;
    double cyclePpm = frames > 0 ? (cycleFrames - frames) / frames * 1e6 : 0;
    fprintf(stderr,
            "JackDAW capture in %d: buffers %d, stamp gaps %d (largest step %d us), ran dry %d; "
            "over %.1f s: stamps %+.1f ppm vs frames, cycle %+.1f ppm vs capture "
            "(%+.0f frames); offset %d..%d frames, reported %d%s\n",
            (int)capIn, (int)gDiag.capBufs.exchange(0), (int)gDiag.capGaps.exchange(0),
            (int)gDiag.capMaxStepUs.exchange(0), (int)gDiag.capDry.exchange(0), span / 1e6,
            stampPpm, cyclePpm, cycleFrames - frames,
            (int)gDiag.capLatMin.exchange(INT32_MAX), (int)gDiag.capLatMax.exchange(0),
            (int)gNode->fInputs[capIn].latencyFrames.load(std::memory_order_relaxed),
            gHoldLatency.load(std::memory_order_relaxed) ? " (held)" : "");
}

// ---- the JdBackend table ----

int mk_open(const char *clientName, const JdBackendCallbacks *cb, void *arg, char *err,
            size_t errLen)
{
    (void)clientName; // the node is always called "JackDAW"
    if (gNode)
        return 0;
    if (!BMediaRoster::IsRunning()) {
        snprintf(err, errLen,
                 "The media services are not running.\n"
                 "Start them from the Media preferences, then launch JackDAW.");
        return -1;
    }
    status_t status;
    BMediaRoster *roster = BMediaRoster::Roster(&status);
    if (!roster || status != B_OK) {
        snprintf(err, errLen, "Could not reach the media services: %s.", strerror(status));
        return -1;
    }
    release_dead_capture_connections(roster);

    float rate = 48000.0f;
    int32 period = 512;
    if (!mixer_output_format(roster, &rate, &period))
        fprintf(stderr, "JackDAW: the system mixer has no output; using %.0f Hz, %d frames\n", rate,
                (int)period);
    if (period < 16 || period > 16384) {
        snprintf(err, errLen, "The system mixer's buffer length (%d frames) is not usable.",
                 (int)period);
        return -1;
    }

    if (cb)
        gCb = *cb;
    else
        memset(&gCb, 0, sizeof gCb);
    gArg = arg;

    gNode = new (std::nothrow) JdMediaNode(rate, period);
    if (!gNode) {
        snprintf(err, errLen, "Out of memory.");
        return -1;
    }
    setup_jacks(gNode);
    status = roster->RegisterNode(gNode);
    if (status != B_OK) {
        snprintf(err, errLen, "Could not register the JackDAW media node: %s.", strerror(status));
        gNode->Release();
        gNode = NULL;
        return -1;
    }
    media_node ts;
    if (roster->GetTimeSource(&ts) == B_OK) {
        roster->SetTimeSourceFor(gNode->ID(), ts.node);
        roster->ReleaseNode(ts);
    }

    gWatcher = new (std::nothrow) RosterWatcher();
    if (gWatcher) {
        gWatcher->Run();
        gNode->fWatcher = BMessenger(gWatcher);
        roster->StartWatching(BMessenger(gWatcher));
        // The server's own quit notice only goes to watchers that ask for it
        // by name (BMediaRoster::StartWatching, MediaRoster.cpp).
        roster->StartWatching(BMessenger(gWatcher), B_MEDIA_SERVER_QUIT);
        const char *diag = getenv("JACKDAW_DIAG");
        gDiagOn = diag && *diag && strcmp(diag, "0") != 0;
        if (gDiagOn) {
            BMessage report(MSG_DIAG_REPORT);
            gWatcher->fDiagRunner =
                new (std::nothrow) BMessageRunner(BMessenger(gWatcher), &report, 2000000);
        }
    }
    return 0;
}

int mk_activate(void)
{
    BMediaRoster *roster = BMediaRoster::CurrentRoster();
    if (!gNode || !roster)
        return -1;

    // Master and Metronome go to the system mixer, as JACK's playback_1/2.
    media_node mixer;
    if (roster->GetAudioMixer(&mixer) == B_OK) {
        gNode->fMixerPort.store(mixer.port, std::memory_order_relaxed);
        status_t e1 = connect_jack(roster, 0, mixer);
        status_t e2 = connect_jack(roster, kMetronomeJack, mixer);
        if (e1 != B_OK || e2 != B_OK)
            fprintf(stderr, "JackDAW: connecting to the system mixer: master %s, metronome %s\n",
                    strerror(e1), strerror(e2));
        roster->ReleaseNode(mixer);
    }

    BTimeSource *ts = roster->MakeTimeSourceFor(gNode->Node());
    if (!ts)
        return -1;
    bigtime_t latency = 0;
    gNode->GetLatency(&latency);
    status_t err = roster->StartNode(gNode->Node(), ts->Now() + latency + 1000);
    ts->Release();
    return err == B_OK ? 0 : -1;
}

void mk_close(void)
{
    if (!gNode)
        return;
    BMediaRoster *roster = BMediaRoster::CurrentRoster();
    if (roster) {
        if (gWatcher) {
            roster->StopWatching(BMessenger(gWatcher), B_MEDIA_SERVER_QUIT);
            roster->StopWatching(BMessenger(gWatcher));
        }
        roster->StopNode(gNode->Node(), 0, true);

        // Disconnect wants both node ids; a connection only names ports.
        media_output outs[kMaxJacks * kMaxOutConns];
        int32 n = 0;
        if (roster->GetConnectedOutputsFor(gNode->Node(), outs, kMaxJacks * kMaxOutConns, &n) ==
            B_OK) {
            for (int32 i = 0; i < n; i++) {
                media_node_id consumer = node_for_port(roster, outs[i].destination.port);
                if (consumer > 0)
                    roster->Disconnect(gNode->ID(), outs[i].source, consumer, outs[i].destination);
            }
        }
        media_input ins[kMaxInputs];
        n = 0;
        if (roster->GetConnectedInputsFor(gNode->Node(), ins, kMaxInputs, &n) == B_OK) {
            for (int32 i = 0; i < n; i++) {
                media_node_id producer = node_for_port(roster, ins[i].source.port);
                if (producer > 0)
                    roster->Disconnect(producer, ins[i].source, gNode->ID(), ins[i].destination);
            }
        }
    }
    if (gWatcher) {
        gWatcher->Lock();
        gWatcher->Quit();
        gWatcher = NULL;
    }
    // StopNode is asynchronous, so a cycle may still be running: the port
    // buffers are freed by the node's destructor, after Quit() has waited
    // for its control thread.
    gNode->Release(); // unregisters and deletes it
    gNode = NULL;
    memset(&gCb, 0, sizeof gCb);
}

jd_nframes_t mk_sample_rate(void)
{
    return gNode ? (jd_nframes_t)lrintf(gNode->Rate()) : 0;
}

jd_nframes_t mk_buffer_size(void)
{
    return gNode ? (jd_nframes_t)gNode->Period() : 0;
}

JdPort *mk_port_register(const char *shortName, int type, int direction)
{
    if (!gNode || !shortName)
        return NULL;
    for (int32 p = 0; p < kMaxPorts; p++) {
        Port &port = gNode->fPorts[p];
        if (port.used.load(std::memory_order_acquire))
            continue;
        if (type == JD_PORT_MIDI) {
            if (!port.midi)
                port.midi = (MidiBuffer *)calloc(1, sizeof(MidiBuffer));
            if (!port.midi)
                return NULL;
            port.midi->count = 0;
        } else {
            if (!port.audio)
                port.audio = (float *)calloc((size_t)gNode->MaxPeriod(), sizeof(float));
            if (!port.audio)
                return NULL;
        }
        port.type = type;
        port.direction = (direction & JD_PORT_INPUT) ? JD_PORT_INPUT : JD_PORT_OUTPUT;
        snprintf(port.shortName, sizeof port.shortName, "%s", shortName);
        snprintf(port.fullName, sizeof port.fullName, "%s%s", kClientPrefix, shortName);
        port.route.store(-1);
        port.used.store(1, std::memory_order_release);

        int32 j, c;
        if (type == JD_PORT_AUDIO && port.direction == JD_PORT_OUTPUT &&
            jack_for_port(shortName, &j, &c)) {
            gNode->fJacks[j].portSlot[c].store(p, std::memory_order_release);
            gNode->fJacks[j].active.store(1, std::memory_order_release);
        }
        return (JdPort *)&port;
    }
    return NULL;
}

void mk_port_unregister(JdPort *jp)
{
    Port *port = (Port *)jp;
    if (!gNode || !port)
        return;
    int32 j, c;
    if (port->type == JD_PORT_AUDIO && port->direction == JD_PORT_OUTPUT &&
        jack_for_port(port->shortName, &j, &c)) {
        Jack &jk = gNode->fJacks[j];
        jk.portSlot[c].store(-1, std::memory_order_release);
        // Jack 0 (Master) and the Metronome stay listed; extra jacks go away
        // with their last port.
        bool any = false;
        for (int32 k = 0; k < jk.channels; k++)
            any |= jk.portSlot[k].load() >= 0;
        if (!any && j != 0 && j != kMetronomeJack)
            jk.active.store(0, std::memory_order_release);
    }
    port->route.store(-1, std::memory_order_release);
    // The buffer stays: the cycle may be reading this slot right now.
    port->used.store(0, std::memory_order_release);
}

const char *mk_port_name(JdPort *jp)
{
    return jp ? ((Port *)jp)->fullName : NULL;
}

jd_nframes_t mk_port_capture_latency(JdPort *jp)
{
    Port *port = (Port *)jp;
    if (!gNode || !port)
        return 0;
    int32 r = port->route.load(std::memory_order_acquire);
    if (r < 0)
        return 0;
    int32 i = (r >> 8) & 0xff;
    if (i >= kMaxInputs)
        return 0;
    int32 frames = gNode->fInputs[i].latencyFrames.load(std::memory_order_relaxed);
    return frames > 0 ? (jd_nframes_t)frames : 0;
}

void *mk_port_buffer(JdPort *jp, jd_nframes_t nframes)
{
    (void)nframes; // always the period
    Port *port = (Port *)jp;
    return port->type == JD_PORT_MIDI ? (void *)port->midi : (void *)port->audio;
}

uint32_t mk_midi_event_count(void *buf)
{
    return buf ? ((MidiBuffer *)buf)->count : 0;
}

int mk_midi_event_get(JdMidiEvent *event, void *buf, uint32_t index)
{
    MidiBuffer *m = (MidiBuffer *)buf;
    if (!m || index >= m->count)
        return -1;
    *event = m->events[index];
    return 0;
}

void mk_midi_clear(void *buf)
{
    if (buf)
        ((MidiBuffer *)buf)->count = 0;
}

int mk_midi_event_write(void *buf, jd_nframes_t time, const uint8_t *data, size_t size)
{
    // MIDI output comes with the Midi Kit backend; until then nothing reads
    // these events, so they are dropped here.
    (void)buf;
    (void)time;
    (void)data;
    (void)size;
    return 0;
}

const char **make_name_list(const std::vector<CaptureSource> &srcs)
{
    if (srcs.empty())
        return NULL;
    size_t bytes = (srcs.size() + 1) * sizeof(char *);
    for (size_t i = 0; i < srcs.size(); i++)
        bytes += strlen(srcs[i].name) + 1;
    char **list = (char **)malloc(bytes);
    if (!list)
        return NULL;
    char *text = (char *)(list + srcs.size() + 1);
    for (size_t i = 0; i < srcs.size(); i++) {
        size_t len = strlen(srcs[i].name) + 1;
        memcpy(text, srcs[i].name, len);
        list[i] = text;
        text += len;
    }
    list[srcs.size()] = NULL;
    return (const char **)list;
}

const char **mk_get_ports(int type, int flags)
{
    if (!gNode || type != JD_PORT_AUDIO)
        return NULL; // MIDI comes with the Midi Kit backend
    // Capture sources only: playback is reached by patching JackDAW's outputs
    // (in Cortex, or the automatic connection to the system mixer), so there
    // are no "physical playback ports" for the engine to connect itself to.
    if (!(flags & JD_PORT_OUTPUT))
        return NULL;
    std::vector<CaptureSource> srcs;
    list_capture_sources(srcs);
    return make_name_list(srcs);
}

void mk_free_ports(const char **ports)
{
    free((void *)ports);
}

int mk_connect(const char *source, const char *destination)
{
    Port *port = port_by_full_name(destination);
    if (!port || port->type != JD_PORT_AUDIO || port->direction != JD_PORT_INPUT) {
        fprintf(stderr, "JackDAW media: connect to %s: no such JackDAW capture port\n",
                destination ? destination : "(null)");
        return -1;
    }
    std::vector<CaptureSource> srcs;
    list_capture_sources(srcs);
    const CaptureSource *s = NULL;
    for (size_t i = 0; i < srcs.size() && !s; i++)
        if (strcmp(srcs[i].name, source) == 0)
            s = &srcs[i];
    if (!s) {
        fprintf(stderr, "JackDAW media: connect %s: no such capture source\n", source);
        return -1;
    }

    int32 input = s->input;
    if (input < 0) {
        BAutolock lock(gNode->fInputLock);
        input = find_input_from(s->output.source);
    }
    if (input < 0) {
        // Not connected to us yet: connect that whole output to our free input.
        BMediaRoster *roster = BMediaRoster::CurrentRoster();
        if (!roster)
            return -1;
        media_input free;
        int32 n = 0;
        if (roster->GetFreeInputsFor(gNode->Node(), &free, 1, &n, B_MEDIA_RAW_AUDIO) != B_OK ||
            n < 1) {
            fprintf(stderr, "JackDAW media: connect %s: JackDAW has no free input\n", source);
            return -1;
        }
        media_format fmt;
        fmt.type = B_MEDIA_RAW_AUDIO;
        fmt.u.raw_audio = media_raw_audio_format::wildcard;
        media_output madeOut;
        media_input madeIn;
        status_t err = roster->Connect(s->output.source, free.destination, &fmt, &madeOut, &madeIn);
        if (err != B_OK) {
            fprintf(stderr, "JackDAW media: connecting %s to JackDAW failed: %s\n", source,
                    strerror(err));
            return -1;
        }
        BAutolock lock(gNode->fInputLock);
        input = find_input_from(madeOut.source);
        if (input < 0)
            return -1;
        gNode->fInputs[input].madeByUs = true;
        gNode->fInputs[input].producerNode = s->node;
    }
    BAutolock lock(gNode->fInputLock);
    const Input &in = gNode->fInputs[input];
    if (!in.connected || s->channel >= in.channels) {
        fprintf(stderr, "JackDAW media: connect %s: input %d is %s with %d channels\n", source,
                (int)input, in.connected ? "connected" : "not connected", (int)in.channels);
        return -1;
    }
    port->route.store(((int32)in.generation << 16) | (input << 8) | s->channel,
                      std::memory_order_release);
    return 0;
}

int mk_disconnect(const char *source, const char *destination)
{
    (void)source; // a capture port reads one source, so the port says which
    Port *port = port_by_full_name(destination);
    if (!port || port->direction != JD_PORT_INPUT)
        return -1;
    int32 r = port->route.exchange(-1, std::memory_order_acq_rel);
    if (r < 0)
        return 0;
    int32 input = (r >> 8) & 0xff;
    if (input >= kMaxInputs)
        return 0;

    // Undo a connection we made once no capture port reads it any more.
    bool stillUsed = false;
    for (int32 p = 0; p < kMaxPorts && !stillUsed; p++) {
        Port &o = gNode->fPorts[p];
        int32 rr = o.route.load(std::memory_order_acquire);
        stillUsed = o.used.load() && rr >= 0 && ((rr >> 8) & 0xff) == input;
    }
    media_source src;
    media_destination dst;
    media_node producer;
    {
        BAutolock lock(gNode->fInputLock);
        Input &in = gNode->fInputs[input];
        if (stillUsed || !in.connected || !in.madeByUs)
            return 0;
        src = in.input.source;
        dst = in.input.destination;
        producer = in.producerNode;
    }
    BMediaRoster *roster = BMediaRoster::CurrentRoster();
    if (roster)
        roster->Disconnect(producer.node, src, gNode->ID(), dst);
    return 0;
}

int mk_create_rt_thread(pthread_t *thread, void *(*fn)(void *), void *arg)
{
    if (pthread_create(thread, NULL, fn, arg) != 0)
        return -1;
    // The same priority as the node's control thread, which runs the cycle.
    set_thread_priority(get_pthread_thread_id(*thread), B_URGENT_PRIORITY);
    return 0;
}

void mk_hold_latency(int hold)
{
    gHoldLatency.store(hold ? 1 : 0, std::memory_order_relaxed);
}

const JdBackend kMediaKitBackend = {
    JD_BACKEND_ABI,
    "Media Kit",
    mk_open,
    mk_activate,
    mk_close,
    mk_sample_rate,
    mk_buffer_size,
    mk_port_register,
    mk_port_unregister,
    mk_port_name,
    mk_port_capture_latency,
    mk_port_buffer,
    mk_midi_event_count,
    mk_midi_event_get,
    mk_midi_clear,
    mk_midi_event_write,
    mk_get_ports,
    mk_free_ports,
    mk_connect,
    mk_disconnect,
    mk_create_rt_thread,
    mk_hold_latency,
};

} // namespace

extern "C" const JdBackend *jackdaw_mediakit_backend_entry(uint32_t abi)
{
    return abi == JD_BACKEND_ABI ? &kMediaKitBackend : NULL;
}
