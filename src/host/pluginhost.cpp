/* pluginhost.cpp — VST3 backend + format dispatch for the jackDAW plugin host.
 *
 * Port of the Linux JackDAW host (pluginhost.c + pluginhost_vst3.cpp) reduced
 * to the VST3 backend, with every X11/GDK editor path replaced by the native
 * Haiku one: plug-ins built against the Haiku-patched SDK can return an
 * IPlugView supporting kPlatformTypeHaikuBView (the host passes a BView* it
 * owns; the plug-in AddChild()s its editor). Plug-ins without an editor keep
 * the parameter-based FX window controls. This TU also owns the public entry
 * points, dispatching PH_LV2 instances to the LV2 backend
 * (pluginhost_lv2.cpp) — the Steinberg SDK headers here and lilv there never
 * meet in one TU.
 *
 * Threading: instantiate/free/params/state/editor run on the owning FX
 * window's looper thread; pluginhost_process() runs on the JACK RT thread.
 * UI → RT parameter changes go through the SDK's lock-free
 * ParameterChangeTransfer ring (the Linux original wrote the RT
 * ParameterChanges from the UI thread — fixed here); RT → UI output parameter
 * changes (meters, DRUMku's learn feedback and pad activity) come back
 * through a second transfer ring drained by pluginhost_ui_poll(). Nothing on
 * the RT path allocates, locks, or logs.
 */

#include "pluginhost.h"
#include "pluginhost_internal.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"

#include "idrumloader.h"
#include "inamfileloader.h"

#include "engine/rt_denormal.h"

#include <Looper.h>        /* BLooper definition for the defensive editor teardown */
#include <MessageRunner.h> /* IRunLoop timer + fd-poll ticks on the looper thread */
#include <Messenger.h>     /* BMessenger target for the run-loop BMessageRunners */
#include <View.h>          /* native editor host view (kPlatformTypeHaikuBView) */
#include <Window.h>        /* BWindow::Lock() around off-looper resizes */

#include <FindDirectory.h> /* B_USER_SETTINGS_DIRECTORY — the scan cache lives there */
#include <StorageDefs.h>

#include <dirent.h>   /* recursive mtime of a .vst3 bundle directory */
#include <errno.h>    /* EINTR while draining the scan helper's stdout */
#include <image.h>    /* Haiku: get_next_image_info → own executable path */
#include <poll.h>     /* IRunLoop fd readiness + the bounded scan-helper wait */
#include <signal.h>   /* SIGKILL for a scan helper that overran its deadline */
#include <sys/stat.h> /* scan-cache validity: plug-in file mtime */
#include <sys/wait.h> /* scan-helper exit status */
#include <unistd.h>   /* read/close on the scan helper's stdout pipe */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

/* ---- Host-wide state (main thread unless noted) ---- */

static double ph_sr = 48000.0;
static int ph_maxblock = 1024;
static GList *ph_cat = NULL; /* PluginInfo* */
static gboolean ph_scanned = FALSE;

/* ---- Diagnostics (JACKDAW_DIAG) ---- */

static __thread int ph_rt_in_callback = 0;
extern "C" void ph_rt_mark(int on)
{
    ph_rt_in_callback = on;
}
extern "C" int ph_rt_active(void)
{
    return ph_rt_in_callback;
}

/* Count IHostApplication::createInstance calls made while the JACK RT thread
 * is inside the process callback: each is a heap allocation (new HostMessage /
 * HostAttributeList) on the RT thread — the classic VST3 metering-message
 * path that breaks the no-RT-malloc rule. RT thread is the single writer. */
static volatile guint64 g_vst3_rt_alloc_calls = 0;
extern "C" guint64 ph_vst3_rt_alloc_count(void)
{
    return g_vst3_rt_alloc_calls;
}

namespace
{

class CountingHostApp : public HostApplication
{
public:
    tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void **obj) override
    {
        if (ph_rt_active())
            g_vst3_rt_alloc_calls++;
        return HostApplication::createInstance(cid, _iid, obj);
    }
};

} // namespace

/* Expose an IHostApplication to plug-ins via the plugin context, registered
 * once before any plug-in code runs. Plug-ins query it during initialize()
 * to create IMessage/IAttributeList objects (NAMku's file paths travel that
 * way); PlugProvider passes it to component+controller initialize(). */
static void ph_ensure_context(void)
{
    static bool done = false;
    if (!done) {
        PluginContextFactory::instance().setPluginContext(new CountingHostApp());
        done = true;
    }
}

/* ---- Backend ---- */

class Vst3ComponentHandler;
class Vst3EditorHostView;

struct Vst3Backend {
    VST3::Hosting::Module::Ptr module;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    IPtr<IAudioProcessor> processor;
    IPtr<NAMku::INamFileLoader> file_loader; /* NULL unless the plug-in has one */
    IPtr<DRUMku::IDrumLoader> drum_loader;   /* NULL unless the plug-in is a drum rack */
    HostProcessData data;
    ProcessContext ctx;
    ParameterChanges in_params;           /* RT-side, pre-allocated */
    ParameterChangeTransfer transfer;     /* UI → RT lock-free ring */
    ParameterChanges out_params;          /* RT-side output changes, pre-allocated */
    ParameterChangeTransfer out_transfer; /* RT → UI lock-free ring */
    EventList in_events{256};             /* MIDI notes, pre-allocated */
    int max_block = 0;
    std::vector<ParamID> param_ids;
    char class_uid[16] = {0}; /* VST3 class CID, for preset-file identity guard */

    /* Native editor (kPlatformTypeHaikuBView). handler receives the editor's
     * performEdit calls; editor_view is the host BView wrapper handed to the
     * FX window (created once per instance, deleted in pluginhost_ui_destroy). */
    Vst3ComponentHandler *handler = nullptr;
    Vst3EditorHostView *editor_view = nullptr;

    /* MIDI CC / pitch-bend delivery (IMidiMapping). Built at load (non-RT):
     * midi_map is the queried interface; cc_param[ch*kCountCtrlNumber + cn] is
     * the ParamID a given channel + controller number maps to, valid when the
     * matching cc_valid byte is set. Read-only on the RT thread. */
    IPtr<IMidiMapping> midi_map;
    std::vector<ParamID> cc_param;
    std::vector<uint8> cc_valid;

    /* Program Change delivery: the kIsProgramChange parameter of the unit
     * IUnitInfo::getUnitByBus names for each channel, and its stepCount, as
     * the SDK's own wrapper does (basewrapper.cpp). kNoParamId = nothing
     * listens. Built at load (non-RT), read-only on the RT thread. */
    ParamID prog_param[16];
    int32 prog_steps[16] = {};
};

/* Host IComponentHandler: receives parameter edits from a plug-in's own
 * editor and forwards them to the RT thread. The editor already updated its
 * controller value before calling performEdit (standard VST3 widget flow), so
 * only the RT hand-off is needed — the same second half of
 * pluginhost_param_set. Window-looper thread only. Ref counting is inert: the
 * handler lives and dies with its backend. */
class Vst3ComponentHandler : public IComponentHandler
{
public:
    explicit Vst3ComponentHandler(Vst3Backend *b) : fBackend(b)
    {
    }
    virtual ~Vst3ComponentHandler() = default;

    tresult PLUGIN_API beginEdit(ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) SMTG_OVERRIDE
    {
        fBackend->transfer.addChange(id, value, 0);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) SMTG_OVERRIDE
    {
        /* Our plug-ins never call this; log so an unexpected request from a
         * third-party plug-in is visible instead of silently ignored. */
        g_message("pluginhost: restartComponent(0x%x) ignored", (unsigned)flags);
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 100;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 100;
    }

private:
    Vst3Backend *fBackend;
};

/* Internal `what` codes for the run-loop BMessageRunners (see the IRunLoop
 * implementation below). Both are delivered to the host view's own handler, so
 * their callbacks run on the FX window looper thread. */
#define PH_MSG_RUNLOOP_POLL 'phrp'  /* poll registered event-handler fds */
#define PH_MSG_RUNLOOP_TIMER 'phrt' /* fire an ITimerHandler ("handler" ptr) */

/* Host BView wrapping a plug-in's IPlugView (kPlatformTypeHaikuBView): the FX
 * window AddChild()s this view; the plug-in view attaches into it while the
 * window looper is locked (AttachedToWindow/DetachedFromWindow both run under
 * the lock). Also the IPlugFrame for plug-in-driven resizes. Owned by the
 * backend; deleted in pluginhost_ui_destroy after the FX window removed it.
 *
 * Additionally implements Steinberg::Linux::IRunLoop. Native Haiku plug-ins do
 * not need it (each BWindow owns a looper thread; they use BMessageRunner for
 * timers). But the Wine-bridge plug-in proxy is derived from a Linux host and
 * queries this IPlugFrame for IRunLoop so it can defer IPlugFrame::resizeView()
 * and context-menu work onto a host-owned GUI thread instead of its IPC read
 * thread; without it the bridge falls back to calling those directly off-thread.
 * We satisfy the contract with looper-thread machinery: a BMessageRunner polls
 * the registered event-handler fds and BMessageRunners drive the timers, so
 * every onFDIsSet()/onTimer() callback runs on this view's looper thread — the
 * thread the bridge needs. All IRunLoop methods are only ever called on that
 * looper thread (the bridge registers/unregisters from IPlugView::setFrame(),
 * which the host drives under the window lock), so no locking is needed. */
class Vst3EditorHostView : public BView, public IPlugFrame, public Steinberg::Linux::IRunLoop
{
public:
    Vst3EditorHostView(IPlugView *view, BRect frame)
        : BView(frame, "vst3-editor", B_FOLLOW_NONE, 0), fPlugView(view)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
        SetExplicitMinSize(BSize(frame.Width(), frame.Height()));
        SetExplicitMaxSize(BSize(frame.Width(), frame.Height()));
        SetExplicitPreferredSize(BSize(frame.Width(), frame.Height()));
    }

    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        if (fPlugView && !fAttached) {
            fPlugView->setFrame(this);
            if (fPlugView->attached(this, kPlatformTypeHaikuBView) == kResultOk)
                fAttached = true;
            else
                g_warning("pluginhost: IPlugView::attached() failed");
        }
    }

    void DetachedFromWindow() override
    {
        DetachPlugView();
        BView::DetachedFromWindow();
    }

    /* Run-loop ticks (poll fds / fire timers) are delivered here, so they run on
     * this view's looper thread — exactly where the bridge needs its deferred
     * IPlugFrame/context work to happen. */
    void MessageReceived(BMessage *msg) override
    {
        switch (msg->what) {
            case PH_MSG_RUNLOOP_POLL:
                RunLoopPollFds();
                break;
            case PH_MSG_RUNLOOP_TIMER: {
                void *h = nullptr;
                if (msg->FindPointer("handler", &h) == B_OK)
                    RunLoopFireTimer(static_cast<Steinberg::Linux::ITimerHandler *>(h));
                break;
            }
            default:
                BView::MessageReceived(msg);
                break;
        }
    }

    void DetachPlugView()
    {
        if (fPlugView && fAttached) {
            fAttached = false;
            fPlugView->setFrame(nullptr);
            fPlugView->removed();
        }
    }

    /* Final teardown (pluginhost_ui_destroy / pluginhost_free). */
    void ReleasePlugView()
    {
        DetachPlugView();
        /* A well-behaved bridge unregisters everything from setFrame(nullptr)
         * above; drop any stragglers so no BMessageRunner outlives this view. */
        StopRunLoop();
        if (fPlugView) {
            fPlugView->release();
            fPlugView = nullptr;
        }
    }

    ~Vst3EditorHostView() override
    {
        StopRunLoop();
    }

    /* IPlugFrame — plug-in-driven resize.
     *
     * This is NOT necessarily the window looper thread. A native plug-in calls
     * it from the looper, but a bridged one delivers it over IPC and it arrives
     * on whichever thread that bridge reads on, so the window is locked here
     * rather than assumed. BView::ResizeBy() and friends call debugger() when
     * the looper is not locked, which kills the whole team.
     *
     * The lock is released before onSize(), which calls back into the plug-in
     * and may block; the host's window must not be held across that. */
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *newSize) SMTG_OVERRIDE
    {
        if (!view || !newSize)
            return kInvalidArgument;
        float w = (float)newSize->getWidth() - 1, h = (float)newSize->getHeight() - 1;

        /* Window() is NULL until the view is attached, and BView only checks the
         * lock when it has an owner, so an unattached view resizes safely. */
        BWindow *window = Window();
        if (window != NULL && !window->Lock())
            return kInternalError;

        SetExplicitMinSize(BSize(w, h));
        SetExplicitMaxSize(BSize(w, h));
        SetExplicitPreferredSize(BSize(w, h));
        ResizeTo(w, h);

        if (window != NULL) {
            window->Unlock();
            /* Re-fit the FX window chrome around the resized editor. The view is
             * already sized, so no width/height payload is needed; PostMessage is
             * thread-safe (this may not be the looper thread). */
            window->PostMessage(PH_MSG_EDITOR_RESIZED);
        }

        return view->onSize(newSize);
    }

    tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame *>(this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, Steinberg::Linux::IRunLoop::iid)) {
            *obj = static_cast<Steinberg::Linux::IRunLoop *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 100;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 100;
    }

    /* ---- Steinberg::Linux::IRunLoop ----
     *
     * All four entry points are called only on this view's looper thread (the
     * bridge registers/unregisters from IPlugView::setFrame(), driven by the
     * host under the window lock), and onFDIsSet()/onTimer() are dispatched from
     * MessageReceived() on that same thread, so the handler lists need no lock. */
    tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *handler,
                                            Steinberg::Linux::FileDescriptor fd) SMTG_OVERRIDE
    {
        if (!handler || fd < 0)
            return kInvalidArgument;
        for (auto &e : fEventHandlers)
            if (e.second == fd)
                return kInvalidArgument; /* one handler per fd (SDK convention) */
        fEventHandlers.push_back({handler, fd});
        if (!fPollRunner) {
            /* ~60 Hz — far faster than any editor resize/context-menu cadence,
             * and only ticking while an event handler is registered. */
            BMessage tick(PH_MSG_RUNLOOP_POLL);
            fPollRunner = new BMessageRunner(BMessenger(this), &tick, 16000 /*µs*/);
            if (fPollRunner->InitCheck() != B_OK) {
                delete fPollRunner;
                fPollRunner = nullptr;
                fEventHandlers.pop_back();
                return kInternalError;
            }
        }
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *handler)
        SMTG_OVERRIDE
    {
        if (!handler)
            return kInvalidArgument;
        bool removed = false;
        for (size_t i = 0; i < fEventHandlers.size(); ++i)
            if (fEventHandlers[i].first == handler) {
                fEventHandlers.erase(fEventHandlers.begin() + i);
                removed = true;
                break;
            }
        if (fEventHandlers.empty() && fPollRunner) {
            delete fPollRunner;
            fPollRunner = nullptr;
        }
        return removed ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *handler,
                                     Steinberg::Linux::TimerInterval ms) SMTG_OVERRIDE
    {
        if (!handler || ms == 0)
            return kInvalidArgument;
        BMessage tick(PH_MSG_RUNLOOP_TIMER);
        tick.AddPointer("handler", handler);
        BMessageRunner *r = new BMessageRunner(BMessenger(this), &tick, (bigtime_t)ms * 1000);
        if (r->InitCheck() != B_OK) {
            delete r;
            return kInternalError;
        }
        fTimers.push_back({handler, r});
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *handler) SMTG_OVERRIDE
    {
        if (!handler)
            return kInvalidArgument;
        for (size_t i = 0; i < fTimers.size(); ++i)
            if (fTimers[i].first == handler) {
                delete fTimers[i].second;
                fTimers.erase(fTimers.begin() + i);
                return kResultTrue;
            }
        return kResultFalse;
    }

private:
    /* Non-blocking readiness check over the registered fds, dispatching
     * onFDIsSet() for each ready one. Walks a snapshot so a handler that
     * unregisters itself from within onFDIsSet() cannot invalidate the walk. */
    void RunLoopPollFds()
    {
        if (fEventHandlers.empty())
            return;
        std::vector<struct pollfd> pfds;
        std::vector<Steinberg::Linux::IEventHandler *> handlers;
        pfds.reserve(fEventHandlers.size());
        handlers.reserve(fEventHandlers.size());
        for (auto &e : fEventHandlers) {
            struct pollfd p = {e.second, POLLIN, 0};
            pfds.push_back(p);
            handlers.push_back(e.first);
        }
        if (poll(pfds.data(), pfds.size(), 0) <= 0)
            return;
        for (size_t i = 0; i < pfds.size(); ++i)
            if (pfds[i].revents & POLLIN)
                handlers[i]->onFDIsSet(pfds[i].fd);
    }

    void RunLoopFireTimer(Steinberg::Linux::ITimerHandler *handler)
    {
        /* Ignore a tick already queued when the timer was unregistered. */
        for (auto &t : fTimers)
            if (t.first == handler) {
                handler->onTimer();
                return;
            }
    }

    void StopRunLoop()
    {
        delete fPollRunner;
        fPollRunner = nullptr;
        fEventHandlers.clear();
        for (auto &t : fTimers)
            delete t.second;
        fTimers.clear();
    }

    IPlugView *fPlugView;
    bool fAttached = false;
    BMessageRunner *fPollRunner = nullptr;
    std::vector<std::pair<Steinberg::Linux::IEventHandler *, int>> fEventHandlers;
    std::vector<std::pair<Steinberg::Linux::ITimerHandler *, BMessageRunner *>> fTimers;
};

/* PluginInstance itself lives in pluginhost_internal.h (shared with the LV2
 * backend TU). */

/* ---- Small helpers ---- */

/* Path validation before loading plugin code: absolute, no "..", sane length. */
static gboolean ph_path_is_safe(const char *path)
{
    if (!path || path[0] != '/')
        return FALSE;
    size_t len = strlen(path);
    if (len < 6 || len > 4000) /* shortest: "/x.vst3" region */
        return FALSE;
    if (strstr(path, ".."))
        return FALSE;
    return TRUE;
}

static std::string ph_self_path(void)
{
    image_info info;
    int32 cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK)
        if (info.type == B_APP_IMAGE)
            return info.name;
    return {};
}

static PluginInfo *ph_info_new(PluginFormat format, const char *key, const char *name,
                               const char *category)
{
    PluginInfo *pi = g_new0(PluginInfo, 1);
    pi->format = format;
    pi->key = g_strdup(key);
    pi->name = g_strdup(name);
    pi->category = g_strdup(category);
    /* VST3/VST2 category strings mark synths as "Instrument|..." */
    pi->is_instrument = category && strstr(category, "Instrument") != NULL;
    return pi;
}

/* ---- Transport (published by the RT thread, read by process_midi) ---- */

static double ph_xport_bpm = 120.0;
static double ph_xport_sr = 48000.0;
static gint64 ph_xport_frame = 0;
static gboolean ph_xport_playing = FALSE;

extern "C" void pluginhost_set_transport(double bpm, double sr, gint64 frame, gboolean playing)
{
    ph_xport_bpm = bpm;
    ph_xport_sr = sr;
    ph_xport_frame = frame;
    ph_xport_playing = playing;
}

/* Read side (VST2 audioMaster callback). Plain reads of the published values;
 * any out-param may be NULL. */
extern "C" void ph_get_transport(double *bpm, double *sr, gint64 *frame, gboolean *playing)
{
    if (bpm)
        *bpm = ph_xport_bpm;
    if (sr)
        *sr = ph_xport_sr;
    if (frame)
        *frame = ph_xport_frame;
    if (playing)
        *playing = ph_xport_playing;
}

static void ph_info_free(gpointer p)
{
    PluginInfo *pi = (PluginInfo *)p;
    g_free(pi->key);
    g_free(pi->name);
    g_free(pi->category);
    g_free(pi);
}

/* ---- Scan ----
 * The main process only ENUMERATES .vst3 bundles and vstplugins files; each is
 * described by a throwaway `JackDAW --scan-plugin <FMT> <path>` helper process,
 * so a module with a crashing static initializer kills the helper, not the DAW.
 * The helper prints one tab-separated "<FMT>\t<name>\t<category>" line per
 * audio-effect class.
 *
 * Results persist in a Reaper-style on-disk cache keyed by path+mtime (port of
 * the Linux tree's plugincache), so only new or changed plug-ins pay for a
 * helper process — which for a Wine-bridged plug-in means starting Wine. LV2 is
 * .ttl-only (lilv) and scans in-process; it uses the cache only for the
 * native-binary check, which otherwise reads every DSP binary in full. */

static void ph_esc(GString *s, const char *v)
{
    for (; v && *v; v++) {
        if (*v == '\\')
            g_string_append(s, "\\\\");
        else if (*v == '\n')
            g_string_append(s, "\\n");
        else if (*v == '\t')
            g_string_append(s, "\\t");
        else
            g_string_append_c(s, *v);
    }
}

static char *ph_unesc(const char *v)
{
    GString *s = g_string_new("");
    for (; v && *v; v++) {
        if (*v == '\\' && v[1]) {
            v++;
            g_string_append_c(s, *v == 'n' ? '\n' : *v == 't' ? '\t' : *v);
        } else {
            g_string_append_c(s, *v);
        }
    }
    return g_string_free(s, FALSE);
}

/* One cached path. `lines` holds the describe helper's raw stdout lines (or the
 * single-flag payload used by the LV2 binary check); empty means "scanned, and
 * it yielded nothing" — a negative result worth remembering so a broken plug-in
 * is not re-probed on every launch. `seen` marks entries touched by the current
 * scan; anything else is dropped on save, which is how uninstalled plug-ins
 * leave the cache. */
typedef struct {
    gint64 mtime;
    GPtrArray *lines;
    gboolean seen;
} PhCacheEnt;

static GHashTable *ph_cache; /* path -> PhCacheEnt; valid only mid-scan */
static void (*ph_progress_cb)(const char *plugin, void *user);
static void *ph_progress_u;

extern "C" void pluginhost_set_scan_progress(void (*cb)(const char *, void *), void *u)
{
    ph_progress_cb = cb;
    ph_progress_u = u;
}

static void ph_cache_ent_free(gpointer p)
{
    PhCacheEnt *e = (PhCacheEnt *)p;
    if (e->lines)
        g_ptr_array_free(e->lines, TRUE);
    g_free(e);
}

/* Both cache files live beside the settings file, in Haiku's per-user settings
 * tree — the directory settings_init() already creates. */
static char *ph_state_path(const char *leaf)
{
    char base[B_PATH_NAME_LENGTH];
    if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, base, sizeof(base)) != B_OK)
        g_strlcpy(base, "/boot/home/config/settings", sizeof(base));
    gchar *dir = g_build_filename(base, "JackDAW", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *out = g_build_filename(dir, leaf, NULL);
    g_free(dir);
    return out;
}

/* Untrusted input (RULES: a corrupt cache must never crash or scribble). Every
 * row that does not parse is skipped; a wrong mtime only forces a rescan. */
static void ph_cache_load(void)
{
    ph_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ph_cache_ent_free);

    char *file = ph_state_path("plugincache");
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(file, &data, &len, NULL)) {
        gchar **rows = g_strsplit(data, "\n", -1);
        for (int i = 0; rows[i]; i++) {
            if (!rows[i][0])
                continue;
            gchar **f = g_strsplit(rows[i], "\t", 3); /* epath  mtime  eline */
            if (f[0] && f[1] && f[2]) {
                char *path = ph_unesc(f[0]);
                PhCacheEnt *e = (PhCacheEnt *)g_hash_table_lookup(ph_cache, path);
                if (!e) {
                    e = g_new0(PhCacheEnt, 1);
                    e->mtime = g_ascii_strtoll(f[1], NULL, 10);
                    e->lines = g_ptr_array_new_with_free_func(g_free);
                    g_hash_table_insert(ph_cache, path, e);
                } else {
                    g_free(path);
                }
                if (f[2][0]) /* an empty payload marks "scanned, nothing found" */
                    g_ptr_array_add(e->lines, ph_unesc(f[2]));
            }
            g_strfreev(f);
        }
        g_strfreev(rows);
        g_free(data);
    }
    g_free(file);
}

static void ph_cache_save(void)
{
    if (!ph_cache)
        return;
    GString *s = g_string_new("");
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, ph_cache);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        PhCacheEnt *e = (PhCacheEnt *)v;
        if (!e->seen) /* not found this scan: the plug-in is gone */
            continue;
        GString *ep = g_string_new("");
        ph_esc(ep, (const char *)k);
        if (e->lines->len == 0) {
            g_string_append_printf(s, "%s\t%lld\t\n", ep->str, (long long)e->mtime);
        } else {
            for (guint i = 0; i < e->lines->len; i++) {
                GString *el = g_string_new("");
                ph_esc(el, (const char *)g_ptr_array_index(e->lines, i));
                g_string_append_printf(s, "%s\t%lld\t%s\n", ep->str, (long long)e->mtime, el->str);
                g_string_free(el, TRUE);
            }
        }
        g_string_free(ep, TRUE);
    }
    char *file = ph_state_path("plugincache");
    g_file_set_contents(file, s->str, s->len, NULL);
    g_free(file);
    g_string_free(s, TRUE);
}

/* Cache validity stamp. VST3 plug-ins are BUNDLE directories, so take the
 * newest mtime found inside (bounded recursion); VST2 plug-ins and LV2 binaries
 * are plain files. 0 on failure — it never matches a stored stamp, so an
 * unreadable path simply rescans. */
static gint64 ph_path_mtime(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode) || depth >= 8)
        return (gint64)st.st_mtime;

    gint64 newest = (gint64)st.st_mtime;
    DIR *d = opendir(path);
    if (!d)
        return newest;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char *child = g_build_filename(path, ent->d_name, NULL);
        gint64 m = ph_path_mtime(child, depth + 1);
        if (m > newest)
            newest = m;
        g_free(child);
    }
    closedir(d);
    return newest;
}

/* Return the entry for `path` if it is present and still current, marking it
 * seen. NULL means the caller must produce fresh content (ph_cache_store). */
static PhCacheEnt *ph_cache_hit(const char *path, gint64 mtime)
{
    if (!ph_cache)
        return NULL;
    PhCacheEnt *e = (PhCacheEnt *)g_hash_table_lookup(ph_cache, path);
    if (!e || e->mtime != mtime)
        return NULL;
    e->seen = TRUE;
    return e;
}

/* Install fresh content for `path`, replacing any stale entry. `lines` may be
 * empty — a remembered negative result. */
static void ph_cache_store(const char *path, gint64 mtime, const char *const *lines, guint n_lines)
{
    if (!ph_cache)
        return;
    PhCacheEnt *e = (PhCacheEnt *)g_hash_table_lookup(ph_cache, path);
    if (!e) {
        e = g_new0(PhCacheEnt, 1);
        e->lines = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(ph_cache, g_strdup(path), e);
    } else {
        g_ptr_array_set_size(e->lines, 0);
    }
    e->mtime = mtime;
    for (guint i = 0; i < n_lines; i++)
        g_ptr_array_add(e->lines, g_strdup(lines[i]));
    e->seen = TRUE;
}

/* Native-binary verdict cache for the LV2 scan (see lph_binary_is_loadable):
 * reading every DSP binary in full is that scan's dominant cost. Only usable
 * while a scan is in progress; a miss returns FALSE and *out is untouched. */
extern "C" gboolean ph_cache_flag_get(const char *path, gboolean *out)
{
    PhCacheEnt *e = ph_cache_hit(path, ph_path_mtime(path, 0));
    if (!e)
        return FALSE;
    *out = (e->lines->len > 0 && ((const char *)g_ptr_array_index(e->lines, 0))[0] == '1');
    return TRUE;
}

extern "C" void ph_cache_flag_set(const char *path, gboolean val)
{
    const char *line = val ? "1" : "0";
    ph_cache_store(path, ph_path_mtime(path, 0), &line, 1);
}

extern "C" int pluginhost_scan_helper_main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "--scan-plugin") != 0)
        return -1; /* not a scan invocation: continue normal startup */
    const char *fmt = argv[2];
    const char *path = argv[3];
    if (!ph_path_is_safe(path))
        return 1;

    if (strcmp(fmt, "VST2") == 0) {
        /* One line per plug-in, printed by the clean VST2 TU (no SDK here). */
        ph_vst2_describe(path);
        return 0;
    }
    if (strcmp(fmt, "VST3") != 0)
        return 1;

    ph_ensure_context();
    std::string err;
    auto mod = VST3::Hosting::Module::create(path, err);
    if (!mod) {
        fprintf(stderr, "scan: could not load '%s': %s\n", path, err.c_str());
        return 1;
    }
    auto factory = mod->getFactory();
    for (auto &ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass)
            continue;
        std::string name = ci.name();
        std::string subcat = ci.subCategoriesString();
        printf("VST3\t%s\t%s\n", name.c_str(), subcat.empty() ? "VST3" : subcat.c_str());
    }
    return 0;
}

/* A wedged helper must not stall the scan: the startup scan runs behind a
 * progress window, where an unbounded wait looks like a hung launch. Generous
 * all the same: a Windows plug-in loads through Wine and vstbridge, and an
 * iLok-protected one took 20 s before its factory was ready (Linux JackDAW,
 * on a Pi 5). */
#define PH_SCAN_TIMEOUT_MS 60000
#define PH_SCAN_MAX_OUTPUT (1u << 20) /* untrusted child output, bounded */

/* Run the describe helper for one plug-in. Returns its stdout, or an empty
 * string when the helper ran but produced nothing usable (crashed, refused the
 * file, or overran the deadline) — a negative result the caller caches so a
 * broken plug-in is not re-probed on every launch. NULL means the helper could
 * not be started at all, which is not cached. Caller frees.
 *
 * The child is read to EOF against a deadline rather than waited on: for a
 * Wine-bridged plug-in the child spawns a Wine host that inherits its stdout,
 * so EOF is the honest "everything it started is done" signal. */
static gchar *ph_run_scan_helper(const char *fmt, const char *path)
{
    std::string self = ph_self_path();
    if (self.empty())
        return NULL;

    gchar *argv[] = {(gchar *)self.c_str(), (gchar *)"--scan-plugin", (gchar *)fmt, (gchar *)path,
                     NULL};
    GPid pid = 0;
    gint out_fd = -1;
    GSpawnFlags flags = (GSpawnFlags)(G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL);
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, flags, NULL, NULL, &pid, NULL, &out_fd, NULL,
                                  NULL))
        return NULL;

    GString *out = g_string_new("");
    gint64 deadline = g_get_monotonic_time() + (gint64)PH_SCAN_TIMEOUT_MS * 1000;
    gboolean failed = FALSE;
    for (;;) {
        gint64 left_us = deadline - g_get_monotonic_time();
        if (left_us <= 0) {
            failed = TRUE;
            break;
        }
        struct pollfd pfd = {out_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, (int)(left_us / 1000));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            failed = TRUE;
            break;
        }
        if (pr == 0) { /* deadline */
            failed = TRUE;
            break;
        }
        char buf[4096];
        ssize_t n = read(out_fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            failed = TRUE;
            break;
        }
        if (n == 0)
            break; /* EOF: the helper and anything holding its stdout are done */
        if (out->len + (gsize)n > PH_SCAN_MAX_OUTPUT) {
            failed = TRUE;
            break;
        }
        g_string_append_len(out, buf, n);
    }
    close(out_fd);

    if (failed) {
        kill((pid_t)pid, SIGKILL);
        g_warning("pluginhost: scan of '%s' did not complete within %d s; skipping it", path,
                  PH_SCAN_TIMEOUT_MS / 1000);
    }
    int status = 0;
    waitpid((pid_t)pid, &status, 0);
    g_spawn_close_pid(pid);

    if (failed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        g_string_free(out, TRUE);
        return g_strdup(""); /* ran, nothing usable — remember that */
    }
    return g_string_free(out, FALSE);
}

/* Describe one plug-in file/bundle and append its catalog entries, reusing the
 * on-disk cache when the path's mtime is unchanged. `fmt` is the format tag
 * ("VST3" or "VST2"); the helper prints "<fmt>\t<name>\t<category>" lines, and
 * those raw lines are what the cache stores. VST3 keys carry the class name
 * (path\nclass — one bundle has many classes); VST2 keys are just the path. */
/* Set during an explicit Rescan: plug-ins cached as failed are probed again. */
static gboolean ph_retry_failed;

extern "C" void ph_scan_via_helper(const char *fmt, const char *path, GList **catalog)
{
    gint64 mtime = ph_path_mtime(path, 0);
    GPtrArray *owned = NULL; /* freshly scanned rows, when it was a cache miss */
    PhCacheEnt *e = ph_cache_hit(path, mtime);
    if (e && ph_retry_failed && e->lines->len == 0)
        e = NULL; /* a remembered failure: probe it again */
    if (!e) {
        if (ph_progress_cb)
            ph_progress_cb(path, ph_progress_u);
        gchar *out = ph_run_scan_helper(fmt, path);
        if (!out)
            return;
        owned = g_ptr_array_new_with_free_func(g_free);
        gchar **split = g_strsplit(out, "\n", -1);
        for (int i = 0; split[i]; i++)
            if (split[i][0])
                g_ptr_array_add(owned, g_strdup(split[i]));
        g_strfreev(split);
        g_free(out);
        /* No-op when no scan is in progress; the rows below are used either way. */
        ph_cache_store(path, mtime, (const char *const *)owned->pdata, owned->len);
    }

    GPtrArray *lines = owned ? owned : e->lines;
    gboolean is_vst3 = (strcmp(fmt, "VST3") == 0);
    PluginFormat pf = is_vst3 ? PH_VST3 : PH_VST2;
    for (guint i = 0; i < lines->len; i++) {
        gchar **f = g_strsplit((const char *)g_ptr_array_index(lines, i), "\t", 3);
        if (f[0] && strcmp(f[0], fmt) == 0 && f[1] && f[2]) {
            gchar *key = is_vst3 ? g_strdup_printf("%s\n%s", path, f[1]) : g_strdup(path);
            *catalog = g_list_prepend(*catalog, ph_info_new(pf, key, f[1], f[2]));
            g_free(key);
        }
        g_strfreev(f);
    }
    if (owned)
        g_ptr_array_free(owned, TRUE);
}

/* Count the paths a scan will describe, so a progress bar can be determinate.
 * Pure enumeration — no plug-in code is loaded. */
extern "C" guint pluginhost_scan_candidate_count(void)
{
    guint n = 0;
    for (auto &module_path : VST3::Hosting::Module::getModulePaths()) {
        (void)module_path;
        n++;
    }
    return n + ph_vst2_count_candidates();
}

static void ph_do_scan(void)
{
    g_list_free_full(ph_cat, ph_info_free);
    ph_cat = NULL;
    ph_cache_load();

    /* Module::getModulePaths() (module_haiku.cpp — the same enumeration the
     * validator's -list uses) walks the canonical Haiku VST3 location
     * (add-ons/media/VST3 from find_paths(), every install root) and returns the
     * .vst3 BUNDLE paths themselves, ready to describe. */
    for (auto &module_path : VST3::Hosting::Module::getModulePaths())
        ph_scan_via_helper("VST3", module_path.c_str(), &ph_cat);
    /* LV2 discovery is pure Turtle parsing via lilv (no plugin code runs), so
     * it happens in-process — the crash-isolating helper is for the binary
     * formats. */
    ph_lv2_scan(&ph_cat);
    /* VST2 plug-ins from add-ons/media/vstplugins (native add-ons + vstbridge
     * stubs); each is described out-of-process, same crash isolation as VST3. */
    ph_vst2_scan(&ph_cat);

    ph_cache_save();
    g_hash_table_destroy(ph_cache);
    ph_cache = NULL;

    ph_cat = g_list_reverse(ph_cat);
    ph_scanned = TRUE;
}

extern "C" const GList *pluginhost_catalog(void)
{
    if (!ph_scanned)
        ph_do_scan();
    return ph_cat;
}

extern "C" void pluginhost_rescan(void)
{
    /* The user's way to retry a plug-in that failed before, such as one that
     * timed out while its licence service was still starting. The startup scan
     * keeps the failure, so a broken plug-in isn't probed on every launch. */
    ph_retry_failed = TRUE;
    ph_do_scan();
    ph_retry_failed = FALSE;
}

/* Persisted baseline of every plugin identity seen on the previous scan, used
 * to tell which plug-ins are newly installed. Separate from plugincache because
 * LV2 is scanned in-process and never populates that cache with catalog rows. */

/* Stable identity for diffing across runs: "<format>:<escaped key>". */
static char *ph_identity(const PluginInfo *pi)
{
    GString *s = g_string_new("");
    g_string_append_printf(s, "%d:", (int)pi->format);
    ph_esc(s, pi->key);
    return g_string_free(s, FALSE);
}

extern "C" GList *pluginhost_scan_report_new(void)
{
    /* Load the previous identity set. On the very first run this stays empty,
     * so every plug-in found is reported as new (seeding the baseline too). */
    GHashTable *prev = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *file = ph_state_path("pluginindex");
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(file, &data, &len, NULL)) {
        gchar **rows = g_strsplit(data, "\n", -1);
        for (int i = 0; rows[i]; i++)
            if (rows[i][0])
                g_hash_table_add(prev, g_strdup(rows[i]));
        g_strfreev(rows);
        g_free(data);
    }

    ph_do_scan();

    /* Diff the fresh catalog against the baseline; collect new names and
     * rebuild the index to persist. */
    GList *new_names = NULL;
    GString *idx = g_string_new("");
    for (const GList *l = ph_cat; l; l = l->next) {
        const PluginInfo *pi = (const PluginInfo *)l->data;
        char *id = ph_identity(pi);
        g_string_append(idx, id);
        g_string_append_c(idx, '\n');
        if (!g_hash_table_contains(prev, id))
            new_names = g_list_prepend(new_names, g_strdup(pi->name));
        g_free(id);
    }
    g_file_set_contents(file, idx->str, idx->len, NULL);
    g_string_free(idx, TRUE);
    g_free(file);
    g_hash_table_destroy(prev);
    return g_list_reverse(new_names);
}

/* ---- Init / shutdown ---- */

extern "C" void pluginhost_init(double sample_rate, int max_block)
{
    ph_sr = sample_rate;
    ph_maxblock = MAX(max_block, 64);
    ph_ensure_context();
    ph_lv2_init(ph_sr, ph_maxblock);
    ph_vst2_init(ph_sr, ph_maxblock);
}

extern "C" void pluginhost_shutdown(void)
{
    g_list_free_full(ph_cat, ph_info_free);
    ph_cat = NULL;
    ph_scanned = FALSE;
    ph_lv2_shutdown();
    ph_vst2_shutdown();
}

/* ---- Instantiate / free ---- */

extern "C" PluginInstance *pluginhost_instantiate(const PluginInfo *info)
{
    if (!info || !info->key)
        return NULL;
    if (info->format == PH_LV2)
        return ph_lv2_instantiate(info);
    if (info->format == PH_VST2)
        return ph_vst2_instantiate(info);
    if (info->format != PH_VST3)
        return NULL;
    ph_ensure_context();

    gchar **parts = g_strsplit(info->key, "\n", 2);
    if (!parts[0] || !parts[1]) {
        g_strfreev(parts);
        return NULL;
    }
    std::string path = parts[0];
    std::string want = parts[1];
    g_strfreev(parts);
    if (!ph_path_is_safe(path.c_str()))
        return NULL;

    std::string err;
    auto module = VST3::Hosting::Module::create(path, err);
    if (!module) {
        g_warning("pluginhost: cannot load '%s': %s", path.c_str(), err.c_str());
        return NULL;
    }
    auto factory = module->getFactory();

    auto infos = factory.classInfos();
    const VST3::Hosting::ClassInfo *found = nullptr;
    for (auto &ci : infos) {
        if (ci.category() == kVstAudioEffectClass && ci.name() == want) {
            found = &ci;
            break;
        }
    }
    if (!found)
        return NULL;

    Vst3Backend *b = new Vst3Backend();
    b->module = module;
    memcpy(b->class_uid, found->ID().data(), 16);
    b->max_block = ph_maxblock;
    b->provider = owned(new PlugProvider(factory, *found, true));
    if (!b->provider->initialize()) {
        delete b;
        return NULL;
    }
    b->component = b->provider->getComponentPtr();
    b->controller = b->provider->getControllerPtr();
    if (!b->component) {
        delete b;
        return NULL;
    }
    b->processor = FUnknownPtr<IAudioProcessor>(b->component);
    if (!b->processor) {
        delete b;
        return NULL;
    }

    /* Stereo in/out arrangement (best effort; a refusing plug-in keeps its
     * default and our buffers follow its actual bus channel counts). */
    SpeakerArrangement in = SpeakerArr::kStereo, out = SpeakerArr::kStereo;
    b->processor->setBusArrangements(&in, 1, &out, 1);

    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = ph_maxblock;
    setup.sampleRate = ph_sr;
    if (b->processor->setupProcessing(setup) != kResultOk)
        g_warning("pluginhost: '%s' refused setupProcessing", want.c_str());

    /* VST3 buses are INACTIVE by default ("The plug-in should only process an
     * activated bus") — without this most plug-ins emit silence. Activate
     * every default-active audio bus (the plug-in's main I/O). */
    for (int d = 0; d < 2; d++) {
        BusDirection bd = (d == 0) ? kInput : kOutput;
        int32 nb = b->component->getBusCount(kAudio, bd);
        for (int32 i = 0; i < nb; i++) {
            BusInfo bi;
            if (b->component->getBusInfo(kAudio, bd, i, bi) == kResultOk &&
                (bi.flags & BusInfo::kDefaultActive))
                b->component->activateBus(kAudio, bd, i, true);
        }
    }
    /* Instruments receive notes on an event INPUT bus, also inactive by
     * default. Activate every default-active one so MIDI reaches the synth. */
    {
        int32 nb = b->component->getBusCount(kEvent, kInput);
        for (int32 i = 0; i < nb; i++) {
            BusInfo bi;
            if (b->component->getBusInfo(kEvent, kInput, i, bi) == kResultOk &&
                (bi.flags & BusInfo::kDefaultActive))
                b->component->activateBus(kEvent, kInput, i, true);
        }
    }

    /* Buffer management by HostProcessData (prepared before activation). */
    if (!b->data.prepare(*b->component, ph_maxblock, kSample32)) {
        delete b;
        return NULL;
    }
    memset(&b->ctx, 0, sizeof(b->ctx));
    b->ctx.sampleRate = ph_sr;
    b->data.processContext = &b->ctx;
    b->data.processMode = kRealtime;
    b->data.symbolicSampleSize = kSample32;
    b->data.inputParameterChanges = &b->in_params;

    if (b->component->setActive(true) != kResultOk) {
        b->data.unprepare();
        delete b;
        return NULL;
    }
    b->processor->setProcessing(true);

    if (b->controller) {
        int32 pc = b->controller->getParameterCount();
        for (int32 i = 0; i < pc; i++) {
            ParameterInfo pinf;
            if (b->controller->getParameterInfo(i, pinf) == kResultOk)
                b->param_ids.push_back(pinf.id);
        }
        /* Pre-allocate both sides of the UI → RT path so the RT thread never
         * grows a queue: the ring, and the ParameterChanges it drains into. */
        b->transfer.setMaxParameters(MAX(256, (int32)b->param_ids.size() * 4));
        b->in_params.setMaxParameters((int32)b->param_ids.size());
        /* ... and of the RT → UI path (processor output parameter changes,
         * drained into out_transfer after each process() call). */
        b->out_params.setMaxParameters((int32)b->param_ids.size());
        b->out_transfer.setMaxParameters(MAX(256, (int32)b->param_ids.size() * 4));
        b->data.outputParameterChanges = &b->out_params;

        /* Receive performEdit from the plug-in's own editor (native BView
         * path); without a handler the editor's knob moves never reach RT. */
        b->handler = new Vst3ComponentHandler(b);
        b->controller->setComponentHandler(b->handler);

        /* Optional file-loading extension (NAMku): discovered purely via
         * queryInterface, so unknown plug-ins are unaffected. */
        NAMku::INamFileLoader *fl = nullptr;
        if (b->controller->queryInterface(NAMku::INamFileLoader_iid, (void **)&fl) == kResultOk &&
            fl)
            b->file_loader = owned(fl);

        /* Optional drum-rack extension (DRUMku): per-slot .wav loading. */
        DRUMku::IDrumLoader *dl = nullptr;
        if (b->controller->queryInterface(DRUMku::IDrumLoader_iid, (void **)&dl) == kResultOk && dl)
            b->drum_loader = owned(dl);

        /* MIDI CC / pitch-bend routing: if the controller implements
         * IMidiMapping, pre-build the (channel, controller#) -> ParamID table so
         * the RT MIDI path can turn CC/bend into input parameter changes without
         * querying (getMidiControllerAssignment is UI-thread only) or allocating.
         * Plug-ins without the interface are unaffected (no table, CC ignored). */
        /* Every answer is checked against the declared parameters: a route to
         * an undeclared id would make addParameterData allocate a new queue on
         * the RT thread, and the message would be lost anyway. */
        std::vector<ParamID> declared(b->param_ids);
        std::sort(declared.begin(), declared.end());
        IMidiMapping *mm = nullptr;
        if (b->controller->queryInterface(IMidiMapping::iid, (void **)&mm) == kResultOk && mm) {
            b->midi_map = owned(mm);
            b->cc_param.assign((size_t)16 * kCountCtrlNumber, 0);
            b->cc_valid.assign((size_t)16 * kCountCtrlNumber, 0);
            for (int16 ch = 0; ch < 16; ch++) {
                for (int cn = 0; cn < kCountCtrlNumber; cn++) {
                    ParamID id = 0;
                    if (mm->getMidiControllerAssignment(0, ch, (CtrlNumber)cn, id) == kResultOk &&
                        id != kNoParamId &&
                        std::binary_search(declared.begin(), declared.end(), id)) {
                        size_t idx = (size_t)ch * kCountCtrlNumber + cn;
                        b->cc_param[idx] = id;
                        b->cc_valid[idx] = 1;
                    }
                }
            }
        }

        /* Program Change: the unit's kIsProgramChange parameter, per channel.
         * A controller without IUnitInfo hears no Program Change. */
        for (int ch = 0; ch < 16; ch++)
            b->prog_param[ch] = kNoParamId;
        IUnitInfo *units = nullptr;
        if (b->controller->queryInterface(IUnitInfo::iid, (void **)&units) == kResultOk && units) {
            for (int ch = 0; ch < 16; ch++) {
                UnitID unit = kRootUnitId;
                if (units->getUnitByBus(kEvent, kInput, 0, ch, unit) != kResultOk)
                    continue;
                for (int32 i = 0; i < pc; i++) {
                    ParameterInfo pinf{};
                    if (b->controller->getParameterInfo(i, pinf) != kResultOk)
                        continue;
                    if ((pinf.flags & ParameterInfo::kIsProgramChange) && pinf.unitId == unit &&
                        pinf.stepCount > 0) {
                        b->prog_param[ch] = pinf.id;
                        b->prog_steps[ch] = pinf.stepCount;
                        break;
                    }
                }
            }
            units->release();
        }
    }

    PluginInstance *pi = g_new0(PluginInstance, 1);
    pi->format = PH_VST3;
    pi->name = g_strdup(info->name);
    pi->key = g_strdup(info->key);
    pi->category = g_strdup(info->category);
    pi->is_instrument =
        info->is_instrument || (info->category && strstr(info->category, "Instrument"));
    pi->active = 1;
    pi->mix_q15 = 32768;
    pi->sample_rate = ph_sr;
    pi->max_block = ph_maxblock;
    pi->dry_L = g_new0(float, ph_maxblock);
    pi->dry_R = g_new0(float, ph_maxblock);
    pi->learn_arm = 0;
    pi->learn_note = -1;
    pi->b = b;
    return pi;
}

extern "C" void pluginhost_free(PluginInstance *inst)
{
    if (!inst)
        return;
    if (inst->format == PH_LV2) {
        ph_lv2_free(inst);
        return;
    }
    if (inst->format == PH_VST2) {
        ph_vst2_free(inst);
        return;
    }
    Vst3Backend *b = inst->b;
    if (b) {
        if (b->editor_view) {
            /* The FX window must pluginhost_ui_destroy before freeing; clean
             * up defensively rather than leak the plug-in view. */
            g_warning("pluginhost: editor view still alive in pluginhost_free");
            if (b->editor_view->Window() && b->editor_view->LockLooper()) {
                BLooper *looper = b->editor_view->Looper();
                b->editor_view->RemoveSelf();
                looper->Unlock();
            }
            b->editor_view->ReleasePlugView();
            delete b->editor_view;
            b->editor_view = nullptr;
        }
        if (b->controller && b->handler)
            b->controller->setComponentHandler(nullptr);
        delete b->handler;
        b->handler = nullptr;
        b->file_loader = nullptr; /* before the controller goes away */
        b->drum_loader = nullptr;
        b->midi_map = nullptr;
        if (b->processor)
            b->processor->setProcessing(false);
        if (b->component)
            b->component->setActive(false);
        b->data.unprepare();
        b->processor = nullptr;
        b->controller = nullptr;
        b->component = nullptr;
        b->provider = nullptr;
        b->module = nullptr;
        delete b;
    }
    g_free(inst->dry_L);
    g_free(inst->dry_R);
    g_free(inst->name);
    g_free(inst->key);
    g_free(inst->category);
    g_free(inst);
}

/* ---- RT processing ---- */

extern "C" gint64 pluginhost_diag_take_max_us(PluginInstance *inst)
{
    if (!inst)
        return 0;
    gint64 v = inst->diag_max_us;
    inst->diag_max_us = 0;
    return v;
}

/* RT: one MIDI-derived value onto parameter id at offset. No allocation:
 * in_params was pre-sized to the parameter count (setMaxParameters) and every
 * id here was checked against the declared parameters at load, so
 * addParameterData reuses a pre-created queue. A queue reserves 5 points
 * (parameterchanges.cpp, kQueueReservedPoints) and a sixth would allocate, so
 * a crowded block lands its latest value on the last point instead (a point at
 * an existing offset is replaced in place). The value also goes to the editor
 * through out_transfer, as the SDK's wrapper sends it to its GUI, so the editor
 * shows what the pedal did. norm is the already-normalized [0,1] value. */
static inline void ph_vst3_midi_param(Vst3Backend *b, ParamID id, ParamValue norm, int32 offset)
{
    int32 qi = 0;
    IParamValueQueue *q = b->in_params.addParameterData(id, qi);
    if (!q)
        return;
    int32 np = q->getPointCount();
    if (np >= 4) {
        int32 last_off = 0;
        ParamValue last_v = 0.0;
        if (q->getPoint(np - 1, last_off, last_v) == kResultOk)
            offset = last_off;
    }
    int32 pi = 0;
    q->addPoint(offset, norm, pi);
    b->out_transfer.addChange(id, norm, offset);
}

/* RT: one MIDI controller (CC#, pitch-bend, channel pressure) as the change of
 * the parameter IMidiMapping assigned it, from the table built at load. */
static inline void ph_deliver_controller(Vst3Backend *b, int channel, int ctrl_num, double norm,
                                         int32 offset)
{
    if (b->cc_valid.empty() || channel < 0 || channel > 15 || ctrl_num < 0 ||
        ctrl_num >= kCountCtrlNumber)
        return;
    size_t idx = (size_t)channel * kCountCtrlNumber + ctrl_num;
    if (!b->cc_valid[idx])
        return;
    ph_vst3_midi_param(b, b->cc_param[idx], norm, offset);
}

/* RT: this block's MIDI into the VST3 process data. VST3 delivers only notes
 * as events (in_events, cleared here); CC, channel pressure and pitch bend
 * become changes of the parameters IMidiMapping names, and Program Change of
 * the unit's kIsProgramChange parameter, scaled as the SDK's wrapper does.
 * Called after the UI ring drained into in_params, so the MIDI value wins. */
static void ph_vst3_queue_midi(PluginInstance *inst, Vst3Backend *b, const PhMidiEvent *ev,
                               int n_ev)
{
    b->in_events.clear();
    for (int i = 0; i < n_ev; i++) {
        const guint8 *m = ev[i].data;
        if (ev[i].size < 1)
            continue;
        uint8 status = m[0] & 0xF0, ch = m[0] & 0x0F;
        uint8 d1 = ev[i].size > 1 ? (m[1] & 0x7F) : 0;
        uint8 d2 = ev[i].size > 2 ? (m[2] & 0x7F) : 0;
        int32 off = (int32)ev[i].time;
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = off;
        e.flags = Event::kIsLive;
        if (status == 0x90 && d2 > 0) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = ch;
            e.noteOn.pitch = d1;
            e.noteOn.velocity = d2 / 127.0f;
            e.noteOn.noteId = -1;
            /* Drum-rack MIDI learn: record the first note-on while armed so the
             * UI can bind it to the slot the user is assigning. Runs only on
             * this RT thread, so a plain get-then-set keeps the first note. */
            if (g_atomic_int_get(&inst->learn_arm) && g_atomic_int_get(&inst->learn_note) < 0)
                g_atomic_int_set(&inst->learn_note, (gint)d1);
            b->in_events.addEvent(e);
        } else if (status == 0x80 || status == 0x90) {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = ch;
            e.noteOff.pitch = d1;
            e.noteOff.velocity = (status == 0x80) ? d2 / 127.0f : 0.0f;
            e.noteOff.noteId = -1;
            b->in_events.addEvent(e);
        } else if (status == 0xB0) {
            /* Control change: controller number == raw CC number (0..127). */
            ph_deliver_controller(b, ch, (int)d1, (double)d2 / 127.0, off);
        } else if (status == 0xE0) {
            /* Pitch bend: 14-bit little-endian (LSB, MSB) -> normalized 0..1. */
            int bend = (int)d1 | ((int)d2 << 7);
            ph_deliver_controller(b, ch, kPitchBend, (double)bend / 16383.0, off);
        } else if (status == 0xD0) {
            /* Channel pressure -> kAfterTouch (single data byte). */
            ph_deliver_controller(b, ch, kAfterTouch, (double)d1 / 127.0, off);
        } else if (status == 0xC0) {
            int32 steps = b->prog_steps[ch];
            if (steps > 0 && d1 <= steps)
                ph_vst3_midi_param(b, b->prog_param[ch], (double)d1 / (double)steps, off);
        }
    }
}

extern "C" void pluginhost_process(PluginInstance *inst, float *L, float *R, int nframes)
{
    pluginhost_process_fx_midi(inst, NULL, 0, L, R, nframes);
}

extern "C" void pluginhost_process_fx_midi(PluginInstance *inst, const PhMidiEvent *ev, int n_ev,
                                           float *L, float *R, int nframes)
{
    if (!inst)
        return;
    if (inst->format == PH_LV2) {
        ph_lv2_process_fx_midi(inst, ev, n_ev, L, R, nframes);
        return;
    }
    if (inst->format == PH_VST2) {
        ph_vst2_process_fx_midi(inst, ev, n_ev, L, R, nframes);
        return;
    }
    if (!inst->b || !inst->b->processor)
        return;
    if (!g_atomic_int_get(&inst->active))
        return; /* bypassed: leave L/R as-is */
    Vst3Backend *b = inst->b;
    if (nframes > b->max_block)
        return;

    int mq = g_atomic_int_get(&inst->mix_q15);
    gboolean blend = (mq < 32768) && inst->dry_L && inst->dry_R;
    if (blend) { /* stash the dry signal for the mix */
        memcpy(inst->dry_L, L, sizeof(float) * (size_t)nframes);
        memcpy(inst->dry_R, R, sizeof(float) * (size_t)nframes);
    }

    /* Re-arm FTZ/DAZ: the previous plugin (or this one) may have cleared
     * MXCSR, which would let denormals stall this plugin's process(). */
    rt_set_denormal_mode();
    gint64 t0 = ph_diag_enabled_i() ? ph_now_us_i() : 0;

    b->data.numSamples = nframes;
    if (b->data.inputs && b->data.inputs[0].channelBuffers32) {
        memcpy(b->data.inputs[0].channelBuffers32[0], L, sizeof(float) * (size_t)nframes);
        if (b->data.inputs[0].numChannels > 1)
            memcpy(b->data.inputs[0].channelBuffers32[1], R, sizeof(float) * (size_t)nframes);
    }
    /* Zero the outputs first: if the plugin fails to process we get silence
     * rather than stale garbage. */
    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        for (int ch = 0; ch < b->data.outputs[0].numChannels; ch++)
            memset(b->data.outputs[0].channelBuffers32[ch], 0, sizeof(float) * (size_t)nframes);
    }

    /* Drain the UI-side ring into the pre-allocated RT ParameterChanges, then
     * this block's control-input MIDI (its CC and Program Change are parameter
     * changes too, so they go in after the UI's). */
    b->transfer.transferChangesTo(b->in_params);
    if (n_ev > 0) {
        ph_vst3_queue_midi(inst, b, ev, n_ev);
        b->data.inputEvents = &b->in_events;
    }
    b->processor->process(b->data);
    b->in_params.clearQueue();
    b->in_events.clear();
    b->data.inputEvents = nullptr;
    /* Publish the plug-in's output parameter changes to the UI-side ring
     * (drained by pluginhost_ui_poll). Both sides pre-allocated. */
    b->out_transfer.transferChangesFrom(b->out_params);
    b->out_params.clearQueue();

    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        memcpy(L, b->data.outputs[0].channelBuffers32[0], sizeof(float) * (size_t)nframes);
        int oc = b->data.outputs[0].numChannels;
        memcpy(R, b->data.outputs[0].channelBuffers32[oc > 1 ? 1 : 0],
               sizeof(float) * (size_t)nframes);
    }

    if (t0) {
        gint64 d = ph_now_us_i() - t0;
        if (d > inst->diag_max_us)
            inst->diag_max_us = d;
    }

    /* Safety net: replace NaN/inf, clamp runaway magnitude (shared helper —
     * every backend's process path must end with it). */
    ph_safety_clamp(L, R, nframes);

    if (blend) { /* wet/dry crossfade */
        float wet = mq * (1.0f / 32768.0f);
        float dry = 1.0f - wet;
        for (int i = 0; i < nframes; i++) {
            L[i] = inst->dry_L[i] * dry + L[i] * wet;
            R[i] = inst->dry_R[i] * dry + R[i] * wet;
        }
    }
}

extern "C" gboolean pluginhost_is_instrument(PluginInstance *inst)
{
    return inst ? inst->is_instrument : FALSE;
}

extern "C" void pluginhost_process_midi(PluginInstance *inst, const PhMidiEvent *ev, int n_ev,
                                        float *L, float *R, int nframes)
{
    if (!inst)
        return;
    if (inst->format == PH_VST2) {
        ph_vst2_process_midi(inst, ev, n_ev, L, R, nframes);
        return;
    }
    /* LV2 instruments are not hosted yet (no MIDI→atom forge; they are
     * excluded from the catalog) — leave the caller's silent buffers as-is. */
    if (inst->format != PH_VST3 || !inst->b || !inst->b->processor)
        return;
    if (!g_atomic_int_get(&inst->active))
        return; /* bypassed: leave the caller's (silent) buffers as-is */
    Vst3Backend *b = inst->b;
    if (nframes > b->max_block)
        return;

    rt_set_denormal_mode();
    gint64 t0 = ph_diag_enabled_i() ? ph_now_us_i() : 0;

    b->data.inputEvents = &b->in_events;

    /* Transport/tempo for the plug-in's ProcessContext (arps, tempo-synced
     * LFOs). Published by the engine at the top of this same RT cycle. */
    b->ctx.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
    if (ph_xport_playing)
        b->ctx.state |= ProcessContext::kPlaying;
    b->ctx.sampleRate = ph_xport_sr;
    b->ctx.tempo = ph_xport_bpm;
    b->ctx.projectTimeSamples = (TSamples)ph_xport_frame;
    double fpb = (ph_xport_bpm > 0.0) ? ph_xport_sr * 60.0 / ph_xport_bpm : 0.0;
    b->ctx.projectTimeMusic = (fpb > 0.0) ? (double)ph_xport_frame / fpb : 0.0;

    b->data.numSamples = nframes;
    if (b->data.inputs && b->data.inputs[0].channelBuffers32) /* silence in */
        for (int ch = 0; ch < b->data.inputs[0].numChannels; ch++)
            memset(b->data.inputs[0].channelBuffers32[ch], 0, sizeof(float) * (size_t)nframes);
    if (b->data.outputs && b->data.outputs[0].channelBuffers32)
        for (int ch = 0; ch < b->data.outputs[0].numChannels; ch++)
            memset(b->data.outputs[0].channelBuffers32[ch], 0, sizeof(float) * (size_t)nframes);

    /* The UI's queued changes, then this block's MIDI: notes as events, and
     * CC / Program Change as parameter changes after the UI's. */
    b->transfer.transferChangesTo(b->in_params);
    ph_vst3_queue_midi(inst, b, ev, n_ev);
    b->processor->process(b->data);
    b->in_params.clearQueue();
    b->out_transfer.transferChangesFrom(b->out_params);
    b->out_params.clearQueue();
    b->in_events.clear();
    b->data.inputEvents = nullptr; /* reset for the effect (audio) path */

    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        memcpy(L, b->data.outputs[0].channelBuffers32[0], sizeof(float) * (size_t)nframes);
        int oc = b->data.outputs[0].numChannels;
        memcpy(R, b->data.outputs[0].channelBuffers32[oc > 1 ? 1 : 0],
               sizeof(float) * (size_t)nframes);
    }

    if (t0) {
        gint64 d = ph_now_us_i() - t0;
        if (d > inst->diag_max_us)
            inst->diag_max_us = d;
    }

    /* Same speaker-safety net as pluginhost_process. */
    ph_safety_clamp(L, R, nframes);
}

/* ---- Reset / state ---- */

extern "C" void pluginhost_reset(PluginInstance *inst)
{
    if (!inst)
        return;
    if (inst->format == PH_LV2) {
        ph_lv2_reset(inst);
        return;
    }
    if (inst->format == PH_VST2) {
        ph_vst2_reset(inst);
        return;
    }
    if (!inst->b)
        return;
    Vst3Backend *b = inst->b;
    /* Inactive→active cycle resets the processor's internal state. */
    if (b->processor)
        b->processor->setProcessing(false);
    if (b->component) {
        b->component->setActive(false);
        b->component->setActive(true);
    }
    if (b->processor)
        b->processor->setProcessing(true);
}

/* Container: two little-endian length-prefixed blobs —
 * [int32 comp_len][comp bytes][int32 ctrl_len][ctrl bytes]. The component
 * stream is what the parameter list cannot capture (NAMku's .nam/.wav paths,
 * internal modes); the controller stream is controller-private extras. */
extern "C" gboolean pluginhost_state_save(PluginInstance *inst, void **out, gsize *out_len)
{
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!inst || !out || !out_len)
        return FALSE;
    if (inst->format == PH_LV2)
        return ph_lv2_state_save(inst, out, out_len);
    if (inst->format == PH_VST2)
        return ph_vst2_state_save(inst, out, out_len);
    if (!inst->b || !inst->b->component)
        return FALSE;
    Vst3Backend *b = inst->b;

    MemoryStream comp, ctrl;
    if (b->component->getState(&comp) != kResultOk)
        return FALSE;
    gboolean have_ctrl = (b->controller && b->controller->getState(&ctrl) == kResultOk);

    gint32 clen = (gint32)comp.getSize();
    gint32 elen = have_ctrl ? (gint32)ctrl.getSize() : 0;
    if (clen < 0 || elen < 0)
        return FALSE;

    gsize total = 8 + (gsize)clen + (gsize)elen;
    guint8 *buf = (guint8 *)g_malloc(total);
    gint32 cle = GINT32_TO_LE(clen), ele = GINT32_TO_LE(elen);
    memcpy(buf + 0, &cle, 4);
    memcpy(buf + 4, &ele, 4);
    if (clen > 0)
        memcpy(buf + 8, comp.getData(), (size_t)clen);
    if (elen > 0)
        memcpy(buf + 8 + clen, ctrl.getData(), (size_t)elen);

    *out = buf;
    *out_len = total;
    return TRUE;
}

extern "C" gboolean pluginhost_state_load(PluginInstance *inst, const void *data, gsize len)
{
    if (!inst || !data)
        return FALSE;
    if (inst->format == PH_LV2)
        return ph_lv2_state_load(inst, data, len);
    if (inst->format == PH_VST2)
        return ph_vst2_state_load(inst, data, len);
    if (!inst->b || !inst->b->component || len < 8)
        return FALSE;
    Vst3Backend *b = inst->b;

    /* The blob is untrusted project-file input: validate the lengths. */
    const guint8 *p = (const guint8 *)data;
    gint32 clen, elen;
    memcpy(&clen, p + 0, 4);
    clen = GINT32_FROM_LE(clen);
    memcpy(&elen, p + 4, 4);
    elen = GINT32_FROM_LE(elen);
    if (clen < 0 || elen < 0 || (gsize)8 + (gsize)clen + (gsize)elen > len)
        return FALSE;

    /* Restore the processor (component) state, then hand the SAME stream to
     * the controller via setComponentState so its parameter view matches. */
    if (clen > 0) {
        MemoryStream cs((void *)(p + 8), clen);
        b->component->setState(&cs);
        cs.seek(0, IBStream::kIBSeekSet, nullptr);
        if (b->controller)
            b->controller->setComponentState(&cs);
    }
    if (elen > 0 && b->controller) {
        MemoryStream es((void *)(p + 8 + clen), elen);
        b->controller->setState(&es);
    }

    /* Mirror the restored normalized values through the UI → RT ring so the
     * live processor matches the controller on the very next block. */
    if (b->controller) {
        for (size_t i = 0; i < b->param_ids.size(); i++) {
            ParamValue v = b->controller->getParamNormalized(b->param_ids[i]);
            b->transfer.addChange(b->param_ids[i], v, 0);
        }
    }
    return TRUE;
}

/* ---- Presets ----
 *
 * A preset file is the same component+controller state blob jackDAW persists per
 * project (pluginhost_state_save), wrapped so it can be reloaded into a fresh
 * instance of the SAME plugin in ANY project. Layout:
 *   [0]  8  magic "JDPRESET"
 *   [8]  4  uint32 LE version (1)
 *   [12] 16 VST3 class CID (must match the target plugin)
 *   [28] .. state blob from pluginhost_state_save
 * Generic to every hosted plugin; for DRUMku the blob is the whole kit (slot count
 * + per-slot path/note/volume), and load re-reads the .wav files via setState. */

#define PH_PRESET_MAGIC "JDPRESET"
#define PH_PRESET_MAGIC_LEN 8
#define PH_PRESET_VERSION 1u
#define PH_PRESET_HDR_LEN (PH_PRESET_MAGIC_LEN + 4 + 16) /* 28 */

/* 16-byte plugin identity for the preset header: the VST3 class CID, or for
 * LV2 the MD5 digest of the identity key (the plugin URI) — same width, same
 * "wrong plugin" rejection semantics. */
static void ph_preset_guard(PluginInstance *inst, guint8 out[16])
{
    memset(out, 0, 16);
    if (inst->format == PH_VST3 && inst->b) {
        memcpy(out, inst->b->class_uid, 16);
        return;
    }
    GChecksum *ck = g_checksum_new(G_CHECKSUM_MD5);
    g_checksum_update(ck, (const guchar *)(inst->key ? inst->key : ""), -1);
    gsize len = 16;
    g_checksum_get_digest(ck, out, &len);
    g_checksum_free(ck);
}

extern "C" gboolean pluginhost_preset_save(PluginInstance *inst, const char *path)
{
    if (!inst || !path || !path[0])
        return FALSE;

    void *blob = NULL;
    gsize blen = 0;
    if (!pluginhost_state_save(inst, &blob, &blen) || !blob)
        return FALSE;

    gsize total = PH_PRESET_HDR_LEN + blen;
    guint8 *buf = (guint8 *)g_malloc(total);
    memcpy(buf, PH_PRESET_MAGIC, PH_PRESET_MAGIC_LEN);
    guint32 ver = GUINT32_TO_LE(PH_PRESET_VERSION);
    memcpy(buf + PH_PRESET_MAGIC_LEN, &ver, 4);
    guint8 guard[16];
    ph_preset_guard(inst, guard);
    memcpy(buf + PH_PRESET_MAGIC_LEN + 4, guard, 16);
    memcpy(buf + PH_PRESET_HDR_LEN, blob, blen);
    g_free(blob);

    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, (const gchar *)buf, (gssize)total, &err);
    g_free(buf);
    if (!ok) {
        g_warning("pluginhost: preset save failed: %s", err ? err->message : "?");
        if (err)
            g_error_free(err);
    }
    return ok;
}

extern "C" gboolean pluginhost_preset_load(PluginInstance *inst, const char *path)
{
    if (!inst || !path || !path[0])
        return FALSE;

    gchar *data = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &data, &len, &err)) {
        g_warning("pluginhost: preset read failed: %s", err ? err->message : "?");
        if (err)
            g_error_free(err);
        return FALSE;
    }

    gboolean ok = FALSE;
    const guint8 *p = (const guint8 *)data;
    if (len < PH_PRESET_HDR_LEN) {
        g_warning("pluginhost: preset too small");
    } else if (memcmp(p, PH_PRESET_MAGIC, PH_PRESET_MAGIC_LEN) != 0) {
        g_warning("pluginhost: not a jackDAW preset");
    } else {
        guint32 ver;
        memcpy(&ver, p + PH_PRESET_MAGIC_LEN, 4);
        ver = GUINT32_FROM_LE(ver);
        guint8 guard[16];
        ph_preset_guard(inst, guard);
        if (ver != PH_PRESET_VERSION) {
            g_warning("pluginhost: unsupported preset version %u", ver);
        } else if (memcmp(p + PH_PRESET_MAGIC_LEN + 4, guard, 16) != 0) {
            g_warning("pluginhost: preset is for a different plugin");
        } else {
            ok = pluginhost_state_load(inst, p + PH_PRESET_HDR_LEN, len - PH_PRESET_HDR_LEN);
        }
    }

    g_free(data);
    return ok;
}

/* ---- Flags / identity ---- */

extern "C" void pluginhost_set_active(PluginInstance *inst, gboolean on)
{
    if (inst)
        g_atomic_int_set(&inst->active, on ? 1 : 0);
}

extern "C" gboolean pluginhost_is_active(PluginInstance *inst)
{
    return inst ? g_atomic_int_get(&inst->active) != 0 : FALSE;
}

extern "C" void pluginhost_set_mix(PluginInstance *inst, float mix)
{
    if (!inst)
        return;
    if (mix < 0.0f)
        mix = 0.0f;
    if (mix > 1.0f)
        mix = 1.0f;
    g_atomic_int_set(&inst->mix_q15, (gint)lrintf(mix * 32768.0f));
}

extern "C" float pluginhost_get_mix(PluginInstance *inst)
{
    return inst ? g_atomic_int_get(&inst->mix_q15) * (1.0f / 32768.0f) : 1.0f;
}

extern "C" const char *pluginhost_name(PluginInstance *inst)
{
    return inst ? inst->name : "";
}

extern "C" PluginFormat pluginhost_format(PluginInstance *inst)
{
    return inst ? inst->format : PH_VST3;
}

extern "C" const char *pluginhost_key(PluginInstance *inst)
{
    return inst ? inst->key : "";
}

extern "C" const char *pluginhost_category(PluginInstance *inst)
{
    return inst ? inst->category : "";
}

/* ---- Parameters (owning window thread only) ---- */

extern "C" guint pluginhost_param_count(PluginInstance *inst)
{
    if (inst && inst->format == PH_LV2)
        return ph_lv2_param_count(inst);
    if (inst && inst->format == PH_VST2)
        return ph_vst2_param_count(inst);
    return (inst && inst->b && inst->b->controller) ? (guint)inst->b->param_ids.size() : 0;
}

extern "C" void pluginhost_param_name(PluginInstance *inst, guint i, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return;
    buf[0] = 0;
    if (inst && inst->format == PH_LV2) {
        ph_lv2_param_name(inst, i, buf, buflen);
        return;
    }
    if (inst && inst->format == PH_VST2) {
        ph_vst2_param_name(inst, i, buf, buflen);
        return;
    }
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return;
    ParameterInfo info;
    if (inst->b->controller->getParameterInfo((int32)i, info) == kResultOk)
        g_strlcpy(buf, StringConvert::convert(info.title).c_str(), (gsize)buflen);
}

extern "C" float pluginhost_param_get(PluginInstance *inst, guint i)
{
    if (inst && inst->format == PH_LV2)
        return ph_lv2_param_get(inst, i);
    if (inst && inst->format == PH_VST2)
        return ph_vst2_param_get(inst, i);
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return 0.0f;
    return (float)inst->b->controller->getParamNormalized(inst->b->param_ids[i]);
}

extern "C" void pluginhost_param_set(PluginInstance *inst, guint i, float v)
{
    if (inst && inst->format == PH_LV2) {
        ph_lv2_param_set(inst, i, v);
        return;
    }
    if (inst && inst->format == PH_VST2) {
        ph_vst2_param_set(inst, i, v);
        return;
    }
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    Vst3Backend *b = inst->b;
    /* Keep the controller's view in sync, then hand the change to the RT
     * thread through the lock-free ring (drained in pluginhost_process). */
    b->controller->setParamNormalized(b->param_ids[i], v);
    b->transfer.addChange(b->param_ids[i], v, 0);
}

extern "C" void pluginhost_param_display(PluginInstance *inst, guint i, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return;
    buf[0] = 0;
    if (inst && inst->format == PH_LV2) {
        ph_lv2_param_display(inst, i, buf, buflen);
        return;
    }
    if (inst && inst->format == PH_VST2) {
        ph_vst2_param_display(inst, i, buf, buflen);
        return;
    }
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return;
    Vst3Backend *b = inst->b;
    ParameterInfo info;
    if (b->controller->getParameterInfo((int32)i, info) != kResultOk)
        return;
    ParamValue norm = b->controller->getParamNormalized(info.id);
    String128 display{};
    std::string text;
    if (b->controller->getParamStringByValue(info.id, norm, display) == kResultOk)
        text = StringConvert::convert(display);
    std::string units = StringConvert::convert(info.units);
    if (!units.empty())
        text += " " + units;
    g_strlcpy(buf, text.c_str(), (gsize)buflen);
}

extern "C" gboolean pluginhost_param_is_stepped(PluginInstance *inst, guint i, gint *steps)
{
    if (steps)
        *steps = 1;
    if (inst && inst->format == PH_LV2)
        return ph_lv2_param_is_stepped(inst, i, steps);
    if (inst && inst->format == PH_VST2)
        return ph_vst2_param_is_stepped(inst, i, steps);
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return FALSE;
    ParameterInfo info;
    if (inst->b->controller->getParameterInfo((int32)i, info) != kResultOk)
        return FALSE;
    if (info.stepCount <= 0)
        return FALSE;
    if (steps)
        *steps = info.stepCount;
    return TRUE;
}

/* ---- Native plugin editor ---- */

extern "C" void *pluginhost_ui_create(PluginInstance *inst)
{
    if (inst && inst->format == PH_LV2)
        return ph_lv2_ui_create(inst);
    if (inst && inst->format == PH_VST2)
        return ph_vst2_ui_create(inst);
    if (!inst || !inst->b || !inst->b->controller)
        return NULL;
    Vst3Backend *b = inst->b;
    if (b->editor_view) /* created once per instance */
        return b->editor_view;

    IPlugView *view = b->controller->createView(ViewType::kEditor);
    if (!view)
        return NULL; /* no editor: FX window falls back to generic controls */
    if (view->isPlatformTypeSupported(kPlatformTypeHaikuBView) != kResultTrue) {
        view->release();
        return NULL;
    }
    ViewRect size;
    if (view->getSize(&size) != kResultOk || size.getWidth() <= 0 || size.getHeight() <= 0) {
        view->release();
        return NULL;
    }
    b->editor_view = new Vst3EditorHostView(
        view, BRect(0, 0, (float)size.getWidth() - 1, (float)size.getHeight() - 1));
    return b->editor_view;
}

extern "C" void pluginhost_ui_poll(PluginInstance *inst)
{
    if (inst && inst->format == PH_LV2) {
        ph_lv2_ui_poll(inst);
        return;
    }
    if (inst && inst->format == PH_VST2) {
        ph_vst2_ui_poll(inst);
        return;
    }
    if (!inst || !inst->b || !inst->b->controller)
        return;
    /* Drain RT → UI output parameter changes into the controller; DRUMku's
     * editor learns about note bindings and pad activity this way. Runs on
     * the FX window's looper thread (the controller's UI thread). */
    Vst3Backend *b = inst->b;
    ParamID id;
    ParamValue value;
    int32 offset;
    while (b->out_transfer.getNextChange(id, value, offset))
        b->controller->setParamNormalized(id, value);
}

extern "C" void pluginhost_ui_destroy(PluginInstance *inst)
{
    if (inst && inst->format == PH_LV2) {
        ph_lv2_ui_destroy(inst);
        return;
    }
    if (inst && inst->format == PH_VST2) {
        ph_vst2_ui_destroy(inst);
        return;
    }
    if (!inst || !inst->b || !inst->b->editor_view)
        return;
    /* The FX window has already RemoveChild()ed the view (which detached the
     * plug-in view via DetachedFromWindow). Final release + delete here. */
    inst->b->editor_view->ReleasePlugView();
    delete inst->b->editor_view;
    inst->b->editor_view = nullptr;
}

/* ---- File loading (INamFileLoader) ---- */

extern "C" gboolean pluginhost_has_file_loader(PluginInstance *inst)
{
    return inst && inst->b && inst->b->file_loader;
}

extern "C" gboolean pluginhost_file_get(PluginInstance *inst, int which, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return FALSE;
    buf[0] = 0;
    if (!pluginhost_has_file_loader(inst))
        return FALSE;
    NAMku::INamFileLoader *fl = inst->b->file_loader;
    tresult r =
        (which == PH_FILE_MODEL) ? fl->getModelFile(buf, buflen) : fl->getIrFile(buf, buflen);
    return r == kResultOk;
}

extern "C" gboolean pluginhost_file_set(PluginInstance *inst, int which, const char *path)
{
    if (!pluginhost_has_file_loader(inst))
        return FALSE;
    NAMku::INamFileLoader *fl = inst->b->file_loader;
    tresult r = (which == PH_FILE_MODEL) ? fl->setModelFile(path ? path : "")
                                         : fl->setIrFile(path ? path : "");
    return r == kResultOk;
}

/* ---- Drum rack (IDrumLoader) ---- */

/* Set/get a parameter by ID (not list index), mirroring pluginhost_param_set:
 * keep the controller's view in sync, then hand the change to the RT thread
 * through the lock-free ring. Values are VST3-normalized [0,1]. */
static void ph_set_param_id(PluginInstance *inst, ParamID id, double norm)
{
    Vst3Backend *b = inst ? inst->b : nullptr;
    if (!b || !b->controller)
        return;
    if (norm < 0.0)
        norm = 0.0;
    if (norm > 1.0)
        norm = 1.0;
    b->controller->setParamNormalized(id, norm);
    b->transfer.addChange(id, norm, 0);
}

static double ph_get_param_id(PluginInstance *inst, ParamID id)
{
    Vst3Backend *b = inst ? inst->b : nullptr;
    if (!b || !b->controller)
        return 0.0;
    return b->controller->getParamNormalized(id);
}

extern "C" gboolean ph_drum_is_rack(PluginInstance *inst)
{
    return inst && inst->b && inst->b->drum_loader;
}

extern "C" gint ph_drum_max_slots(void)
{
    return (gint)DRUMku::kMaxSlots;
}

extern "C" gint ph_drum_slot_count(PluginInstance *inst)
{
    if (!ph_drum_is_rack(inst))
        return 0;
    double norm = ph_get_param_id(inst, (ParamID)DRUMku::kSlotCountId);
    gint plain = (gint)(1.0 + norm * (double)(DRUMku::kMaxSlots - 1) + 0.5);
    if (plain < 1)
        plain = 1;
    if (plain > DRUMku::kMaxSlots)
        plain = DRUMku::kMaxSlots;
    return plain;
}

extern "C" void ph_drum_add_slot(PluginInstance *inst)
{
    if (!ph_drum_is_rack(inst))
        return;
    gint plain = ph_drum_slot_count(inst) + 1;
    if (plain > DRUMku::kMaxSlots)
        plain = DRUMku::kMaxSlots;
    double norm = (double)(plain - 1) / (double)(DRUMku::kMaxSlots - 1);
    ph_set_param_id(inst, (ParamID)DRUMku::kSlotCountId, norm);
}

extern "C" gboolean ph_drum_file_get(PluginInstance *inst, int slot, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return FALSE;
    buf[0] = 0;
    if (!ph_drum_is_rack(inst))
        return FALSE;
    return inst->b->drum_loader->getSampleFile(slot, buf, buflen) == kResultOk;
}

extern "C" gboolean ph_drum_file_set(PluginInstance *inst, int slot, const char *path)
{
    if (!ph_drum_is_rack(inst))
        return FALSE;
    return inst->b->drum_loader->setSampleFile(slot, path ? path : "") == kResultOk;
}

extern "C" float ph_drum_volume_get(PluginInstance *inst, int slot)
{
    if (!ph_drum_is_rack(inst) || slot < 0 || slot >= DRUMku::kMaxSlots)
        return 0.0f;
    return (float)ph_get_param_id(inst, (ParamID)(DRUMku::kSlotVolumeBase + slot));
}

extern "C" void ph_drum_volume_set(PluginInstance *inst, int slot, float v)
{
    if (!ph_drum_is_rack(inst) || slot < 0 || slot >= DRUMku::kMaxSlots)
        return;
    ph_set_param_id(inst, (ParamID)(DRUMku::kSlotVolumeBase + slot), v);
}

extern "C" gint ph_drum_note_get(PluginInstance *inst, int slot)
{
    if (!ph_drum_is_rack(inst) || slot < 0 || slot >= DRUMku::kMaxSlots)
        return -1;
    double norm = ph_get_param_id(inst, (ParamID)(DRUMku::kSlotNoteBase + slot));
    gint plain = (gint)(norm * (double)DRUMku::kNoteUnassigned + 0.5);
    return plain >= DRUMku::kNoteUnassigned ? -1 : plain;
}

extern "C" void ph_drum_note_set(PluginInstance *inst, int slot, gint note)
{
    if (!ph_drum_is_rack(inst) || slot < 0 || slot >= DRUMku::kMaxSlots)
        return;
    gint plain = (note < 0 || note > 127) ? (gint)DRUMku::kNoteUnassigned : note;
    double norm = (double)plain / (double)DRUMku::kNoteUnassigned;
    ph_set_param_id(inst, (ParamID)(DRUMku::kSlotNoteBase + slot), norm);
}

extern "C" void ph_drum_learn_arm(PluginInstance *inst, gboolean on)
{
    if (!inst)
        return;
    g_atomic_int_set(&inst->learn_note, -1);
    g_atomic_int_set(&inst->learn_arm, on ? 1 : 0);
}

extern "C" gint ph_drum_learn_take_note(PluginInstance *inst)
{
    if (!inst)
        return -1;
    gint note = g_atomic_int_get(&inst->learn_note);
    if (note >= 0)
        g_atomic_int_set(&inst->learn_note, -1);
    return note;
}
