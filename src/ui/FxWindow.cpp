#include "FxWindow.h"

#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <Entry.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringItem.h>
#include <StringView.h>

#include <stdio.h>
#include <string.h>

#include "host/pluginhost.h"
#include "MainWindow.h"
#include "Messages.h"

// ---------------------------------------------------------------------------
// Scrollable parameter panel. The parameter list varies wildly between
// plug-ins (a couple of sliders for a delay, dozens for a synth), so rather
// than resize the window to fit — which fights the layout system, because the
// sliders have unbounded maximum width and the window's preferred size then
// tracks its own frame — the panel lives in a vertical scroll view. The
// window keeps a fixed, user-settable size; a plug-in with more parameters
// than fit simply scrolls. This is the Interface Kit's standard idiom for a
// layout-managed group inside a scroll view (see Haiku's HaikuDepot).
// ---------------------------------------------------------------------------
namespace
{

class ParamGroupView : public BGroupView
{
public:
    ParamGroupView() : BGroupView(B_VERTICAL, 4.0f)
    {
    }

    // Let the group be laid out much shorter than its content: the scroll
    // view sizes it to the viewport, and anything taller scrolls.
    BSize MinSize() override
    {
        BSize min = BGroupView::MinSize();
        return BSize(min.width, 60.0f);
    }

protected:
    void DoLayout() override
    {
        BGroupView::DoLayout();
        BScrollBar *scrollBar = ScrollBar(B_VERTICAL);
        if (scrollBar == NULL)
            return;

        // The real content height is the bottom of the last laid-out item
        // (MinSize is not reliable for height-for-width children).
        float contentHeight = GroupLayout()->LayoutArea().Height();
        int32 count = GroupLayout()->CountItems();
        if (count > 0) {
            BLayoutItem *last = GroupLayout()->ItemAt(count - 1);
            if (last != NULL)
                contentHeight = last->Frame().bottom;
        }
        float viewHeight = Bounds().Height();
        float max = contentHeight - viewHeight;
        scrollBar->SetRange(0.0f, max > 0.0f ? max : 0.0f);
        scrollBar->SetProportion(contentHeight > 0.0f ? viewHeight / contentHeight : 1.0f);
        scrollBar->SetSteps(16.0f, viewHeight);
    }
};

class ParamScrollView : public BScrollView
{
public:
    ParamScrollView(const char *name, BView *target)
        : BScrollView(name, target, 0, false, true, B_NO_BORDER)
    {
    }

protected:
    void DoLayout() override
    {
        float sbWidth = be_control_look->GetScrollBarWidth(B_VERTICAL);
        BRect inner = Bounds();
        inner.right -= sbWidth + 1.0f;

        if (BView *target = Target()) {
            target->MoveTo(inner.left, inner.top);
            target->ResizeTo(inner.Width(), inner.Height());
        }
        if (BScrollBar *scrollBar = ScrollBar(B_VERTICAL)) {
            BRect r = inner;
            r.left = r.right + 1.0f;
            r.right = r.left + sbWidth;
            scrollBar->MoveTo(r.left, r.top);
            scrollBar->ResizeTo(r.Width(), r.Height());
        }
    }
};

} // namespace

enum {
    MSG_FX_ADD = 'fxad',    // string "key", "name", "category"
    MSG_FX_SELECT = 'fxsl', // chain list selection changed
    MSG_FX_REMOVE = 'fxrm',
    MSG_FX_UP = 'fxup',
    MSG_FX_DOWN = 'fxdn',
    MSG_FX_BYPASS = 'fxby',
    MSG_FX_MIX = 'fxmx',
    MSG_FX_PARAM = 'fxpm', // int32 "param" (index into the instance's params)
    MSG_FX_RAISE = 'fxrs', // re-activate an already-open window
    MSG_FX_SYNC = 'fxsy',  // MIDI control changed bypass/mix: show the new state

    // Drum rack (DRUMku). All carry int32 "slot" except Add and Poll.
    MSG_DRUM_LOAD = 'dkld',      // open a file panel for a slot
    MSG_DRUM_FILE_REFS = 'dkrf', // BFilePanel result; int32 "slot" from the template
    MSG_DRUM_VOL = 'dkvl',       // a slot's volume slider moved
    MSG_DRUM_LEARN = 'dklr',     // a slot's Learn checkbox toggled
    MSG_DRUM_ADD = 'dkad',       // +Add slot
    MSG_DRUM_POLL = 'dkpl',      // periodic poll for the captured learn note

    // Presets (generic to every plugin): full state ⇄ a .jdpreset file.
    MSG_PRESET_SAVE = 'psav',      // "Save preset…" clicked → show B_SAVE_PANEL
    MSG_PRESET_LOAD = 'plod',      // "Load preset…" clicked → show B_OPEN_PANEL
    MSG_PRESET_SAVE_REFS = 'psrf', // save panel result: entry_ref "directory" + "name"
    MSG_PRESET_LOAD_REFS = 'plrf', // open panel result: entry_ref "refs"

    MSG_FXUI_TICK = 'uitk',    // ~30 Hz host→UI feedback for an embedded editor
    MSG_FX_TOGGLE_UI = 'fxtg', // switch the selected plugin editor ⇄ generic panel
    MSG_FX_COLLECT = 'fxcl',   // one-shot: free the instance retired by a Remove
};

// Delay before collecting a removed plug-in. Only has to outlast the RT
// thread's use of the chain snapshot that still referenced it — two JACK
// periods, i.e. ~5 ms at -p 128 — but the collect is also gated on the engine's
// cycle counter, so this is just a comfortable margin.
static const bigtime_t kFxCollectDelay = 250000; // 250 ms

static const int32 kSliderSteps = 1000; // BSlider is integer; params are [0,1]

// Vertical space around an embedded editor (toggle-button row + add-effect
// toolbar + window insets/spacing) added to the editor's own height when
// sizing the window so the whole editor is visible.
static const float kEditorChromeHeight = 118.0f;

FxWindow::FxWindow(JackDawTrack *track, MainWindow *main)
    : BWindow(BRect(0, 0, 900, 460), "Effects", B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
      m_track(track), m_main(main), m_file_panel(NULL), m_save_panel(NULL), m_preset_save(NULL),
      m_preset_load(NULL), m_learn_slot(-1), m_learn_poll(NULL), m_embedded_ui(NULL),
      m_embedded_inst(NULL), m_ui_poll(NULL), m_collect(NULL)
{
    g_object_ref(m_track);

    char title[128];
    snprintf(title, sizeof(title), "Effects — %s", jackdaw_track_get_name(m_track));
    SetTitle(title);

    m_add_menu = new BPopUpMenu("Add effect…");
    m_add_menu->SetRadioMode(false);
    m_add_field = new BMenuField("add", NULL, m_add_menu);

    m_chain_list = new BListView("chain");
    m_chain_list->SetSelectionMessage(new BMessage(MSG_FX_SELECT));
    BScrollView *chain_scroll =
        new BScrollView("chain-scroll", m_chain_list, 0, false, true, B_FANCY_BORDER);
    chain_scroll->SetExplicitMinSize(BSize(180.0f, 140.0f));

    m_remove = new BButton("remove", "Remove", new BMessage(MSG_FX_REMOVE));
    m_up = new BButton("up", "Up", new BMessage(MSG_FX_UP));
    m_down = new BButton("down", "Down", new BMessage(MSG_FX_DOWN));

    m_bypass = new BCheckBox("bypass", "Bypass", new BMessage(MSG_FX_BYPASS));
    m_mix = new BSlider("mix", "Mix (dry–wet)", new BMessage(MSG_FX_MIX), 0, kSliderSteps,
                        B_HORIZONTAL);
    m_mix->SetModificationMessage(new BMessage(MSG_FX_MIX));
    m_mix->SetValue(kSliderSteps);

    m_preset_save = new BButton("preset-save", "Save preset…", new BMessage(MSG_PRESET_SAVE));
    m_preset_load = new BButton("preset-load", "Load preset…", new BMessage(MSG_PRESET_LOAD));

    m_param_group = new ParamGroupView();
    BScrollView *param_scroll = new ParamScrollView("param-scroll", m_param_group);

    // clang-format off
    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL)
            .Add(m_add_field)
            .AddGlue()
        .End()
        .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
            .AddGroup(B_VERTICAL, 4.0f)
                .Add(chain_scroll)
                .AddGroup(B_HORIZONTAL, 4.0f)
                    .Add(m_remove)
                    .Add(m_up)
                    .Add(m_down)
                    .AddGlue()
                .End()
                .Add(m_bypass)
                .Add(m_mix)
                .AddGroup(B_HORIZONTAL, 4.0f)
                    .Add(m_preset_save)
                    .Add(m_preset_load)
                    .AddGlue()
                .End()
                .AddGlue()
            .End()
            .Add(param_scroll)
        .End();
    // clang-format on

    BuildAddMenu();
    RebuildChainList(jackdaw_track_fx_count(m_track) > 0 ? 0 : -1);
    CenterOnScreen();
}

// One-shot timer that brings us back to free a just-retired instance.
void FxWindow::ArmFxCollect()
{
    delete m_collect;
    BMessage tick(MSG_FX_COLLECT);
    m_collect = new BMessageRunner(BMessenger(this), &tick, kFxCollectDelay, 1);
}

bool FxWindow::QuitRequested()
{
    // Closing right after a Remove must not strand the retired instance (and,
    // for a bridged plug-in, its Wine host) until the track is destroyed. This
    // hook runs on this window's own looper — FX windows are always closed with
    // PostMessage(B_QUIT_REQUESTED) — so it is the right thread to free on, and
    // by now the RT thread has long since cycled past the retired chain.
    jackdaw_track_fx_collect(m_track);
    return BWindow::QuitRequested();
}

FxWindow::~FxWindow()
{
    delete m_collect;
    // Tear every native editor down BEFORE ~BWindow deletes child views: the
    // UI's cleanup() removes and deletes its own view, so a view left
    // attached here would be deleted twice. (Same discipline as the Linux
    // FX window freeing its suil editors before the GTK window dies.)
    DetachEmbeddedUi(false);
    guint n = jackdaw_track_fx_count(m_track);
    for (guint i = 0; i < n; i++)
        pluginhost_ui_destroy((PluginInstance *)jackdaw_track_fx_get(m_track, i));
    delete m_ui_poll;
    delete m_learn_poll;
    delete m_file_panel;
    delete m_save_panel;
    // Deregister from the track + main window under the main lock (idempotent:
    // only if this window is still the registered FX window). Runs on whichever
    // thread called Quit(); the main window's shutdown path drops its own lock
    // while waiting for this rather than locking FX windows (mirrors the piano-
    // roll editor handshake). Lock() can still fail if the main looper is already
    // dead (e.g. an unexpected teardown order) — only unregister/unlock when it
    // succeeded; a dead main window has nothing left to unregister from.
    if (m_main->Lock()) {
        if (g_object_get_data(G_OBJECT(m_track), "fx-window") == this)
            g_object_set_data(G_OBJECT(m_track), "fx-window", NULL);
        m_main->UnregisterFxWindow(this);
        m_main->Unlock();
    }
    g_object_unref(m_track);
}

void FxWindow::NotifyChainChanged()
{
    BMessage m(MSG_FX_CHAIN_CHANGED);
    m.AddPointer("track", m_track);
    BMessenger(m_main).SendMessage(&m);
}

PluginInstance *FxWindow::Selected()
{
    int32 sel = m_chain_list->CurrentSelection();
    if (sel < 0)
        return NULL;
    return (PluginInstance *)jackdaw_track_fx_get(m_track, (guint)sel);
}

/* The catalog is already warm: the app scans it at startup, behind the progress
 * window, so this is a plain list walk and the FX window opens immediately.
 * pluginhost_catalog() would still scan lazily if that ever did not happen. */
void FxWindow::BuildAddMenu()
{
    while (m_add_menu->CountItems() > 0)
        delete m_add_menu->RemoveItem((int32)0);

    const GList *cat = pluginhost_catalog();
    if (!cat) {
        BMenuItem *none = new BMenuItem("No plugins found", NULL);
        none->SetEnabled(false);
        m_add_menu->AddItem(none);
        return;
    }
    for (const GList *l = cat; l; l = l->next) {
        const PluginInfo *pi = (const PluginInfo *)l->data;
        BMessage *msg = new BMessage(MSG_FX_ADD);
        msg->AddInt32("format", (int32)pi->format);
        msg->AddString("key", pi->key);
        msg->AddString("name", pi->name);
        msg->AddString("category", pi->category);
        char label[160];
        snprintf(label, sizeof(label), "%s  (%s)", pi->name, pi->category);
        BMenuItem *item = new BMenuItem(label, msg);
        item->SetTarget(this);
        m_add_menu->AddItem(item);
    }
}

void FxWindow::RebuildChainList(int select)
{
    while (m_chain_list->CountItems() > 0)
        delete m_chain_list->RemoveItem((int32)0);

    guint n = jackdaw_track_fx_count(m_track);
    for (guint i = 0; i < n; i++) {
        PluginInstance *inst = (PluginInstance *)jackdaw_track_fx_get(m_track, i);
        char label[160];
        snprintf(label, sizeof(label), "%u. %s%s", i + 1, pluginhost_name(inst),
                 pluginhost_is_active(inst) ? "" : "  [bypassed]");
        m_chain_list->AddItem(new BStringItem(label));
    }
    if (select >= 0 && select < (int)n)
        m_chain_list->Select(select); // fires MSG_FX_SELECT → panel rebuild
    else
        RebuildParamPanel();
}

void FxWindow::SyncControlsRow()
{
    PluginInstance *inst = Selected();
    m_bypass->SetEnabled(inst != NULL);
    m_mix->SetEnabled(inst != NULL);
    m_remove->SetEnabled(inst != NULL);
    m_up->SetEnabled(inst != NULL);
    m_down->SetEnabled(inst != NULL);
    m_preset_save->SetEnabled(inst != NULL);
    m_preset_load->SetEnabled(inst != NULL);
    if (inst) {
        m_bypass->SetValue(pluginhost_is_active(inst) ? B_CONTROL_OFF : B_CONTROL_ON);
        m_mix->SetValue((int32)(pluginhost_get_mix(inst) * kSliderSteps));
    }
}

void FxWindow::UpdateValueLabel(guint param)
{
    PluginInstance *inst = Selected();
    if (!inst || param >= m_value_labels.size())
        return;
    char buf[128];
    pluginhost_param_display(inst, param, buf, sizeof(buf));
    m_value_labels[param]->SetText(buf);
}

static void fx_basename(const char *path, const char *prefix, char *out, size_t outlen)
{
    const char *base = path && *path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : (path && *path ? path : "(none)");
    snprintf(out, outlen, "%s%s", prefix, base);
}

// Fully tear the parameter panel down. Removing only the child VIEWS (as the
// first version did) leaks the layout items the builder created — the nested
// BGroupLayouts of each row and the trailing glue are BLayoutItems, not views,
// so they accumulate on every rebuild and inflate the panel's reported size.
// Clear both: delete the child views, then any layout items left behind.
// Detach (and optionally destroy) the embedded native editor. Detaching must
// happen before the generic delete loop below ever runs: the view belongs to
// the plugin UI, which deletes it itself in cleanup().
void FxWindow::DetachEmbeddedUi(bool destroy)
{
    delete m_ui_poll;
    m_ui_poll = NULL;
    if (m_embedded_ui) {
        m_param_group->RemoveChild(m_embedded_ui);
        m_embedded_ui = NULL;
    }
    if (destroy && m_embedded_inst)
        pluginhost_ui_destroy(m_embedded_inst);
    m_embedded_inst = NULL;
}

void FxWindow::FitWindowToEmbeddedEditor()
{
    if (!m_embedded_ui)
        return;
    // Backends publish the editor's desired size as the view's explicit min
    // size (at embed and on every plug-in-driven resize).
    BSize want = m_embedded_ui->MinSize();
    float editorH = want.height;
    if (editorH <= 0.0f)
        return;
    // Keep the current width unless the editor now wants to be wider than the
    // space it currently has (the param scroll view scrolls vertically only, so
    // extra width must come from the window). At embed time the view has not
    // been laid out yet (Frame width 0), so width is left unchanged — matching
    // the original embed behavior.
    float targetW = Bounds().Width();
    float have = m_embedded_ui->Frame().Width();
    if (have > 0.0f && want.width > have)
        targetW += want.width - have;
    ResizeTo(targetW, editorH + kEditorChromeHeight);
}

void FxWindow::ClearParamPanel()
{
    DetachEmbeddedUi(false);

    m_sliders.clear();
    m_value_labels.clear();

    // Drop any armed MIDI-learn state (the checkbox views are about to go away).
    // The plug-in's own learn flag is reset when a rack is (re)built.
    delete m_learn_poll;
    m_learn_poll = NULL;
    m_learn_slot = -1;
    m_drum_file.clear();
    m_drum_vol.clear();
    m_drum_learn.clear();
    m_drum_note.clear();

    BView *child;
    while ((child = m_param_group->ChildAt(0)) != NULL) {
        m_param_group->RemoveChild(child);
        delete child;
    }
    BLayout *layout = m_param_group->GroupLayout();
    while (layout->CountItems() > 0)
        delete layout->RemoveItem((int32)0);
}

void FxWindow::RebuildParamPanel()
{
    ClearParamPanel();

    PluginInstance *inst = Selected();
    SyncControlsRow();

    // A plugin that ships a native editor (LV2 HaikuUI or a VST3 IPlugView
    // supporting kPlatformTypeHaikuBView) shows it in place of the generic
    // panels — same behavior as the Linux FX window embedding suil editors —
    // unless the user toggled this instance to the generic controls. The view
    // is created once per instance and re-attached on every selection; a
    // ~30 Hz runner delivers host→UI feedback.
    if (inst && m_prefer_generic.find(inst) == m_prefer_generic.end()) {
        BView *ui = (BView *)pluginhost_ui_create(inst);
        if (ui) {
            BLayoutBuilder::Group<> uiBuilder(m_param_group->GroupLayout());
            // clang-format off
            uiBuilder
                .AddGroup(B_HORIZONTAL, 6.0f)
                    .Add(new BButton("ui-toggle", "Generic controls",
                                     new BMessage(MSG_FX_TOGGLE_UI)))
                    .AddGlue()
                .End()
                .Add(ui)
                .AddGlue();
            // clang-format on
            m_embedded_ui = ui;
            m_embedded_inst = inst;
            // The editor sits in a scroll view that clips a tall editor to the
            // visible height, so grow the window to show the whole editor plus
            // the toggle-button row and the surrounding toolbar/insets chrome.
            FitWindowToEmbeddedEditor();
            BMessage tick(MSG_FXUI_TICK);
            m_ui_poll = new BMessageRunner(BMessenger(this), &tick, 33000);
            return;
        }
    }

    // The way back to the editor exists only for instances toggled away from
    // one (editor-less plugins never enter m_prefer_generic).
    bool editorToggle = inst && m_prefer_generic.find(inst) != m_prefer_generic.end();

    // A drum rack gets a purpose-built per-slot panel instead of the generic
    // slider list.
    if (inst && ph_drum_is_rack(inst)) {
        if (editorToggle) {
            BLayoutBuilder::Group<> tb(m_param_group->GroupLayout());
            // clang-format off
            tb
                .AddGroup(B_HORIZONTAL, 6.0f)
                    .Add(new BButton("ui-toggle", "Plugin editor",
                                     new BMessage(MSG_FX_TOGGLE_UI)))
                    .AddGlue()
                .End();
            // clang-format on
        }
        BuildDrumRack(inst);
        return;
    }

    BLayoutBuilder::Group<> builder(m_param_group->GroupLayout());
    if (!inst) {
        builder.Add(new BStringView("hint", "Select or add an effect."));
        builder.AddGlue();
        return;
    }

    if (editorToggle) {
        // clang-format off
        builder
            .AddGroup(B_HORIZONTAL, 6.0f)
                .Add(new BButton("ui-toggle", "Plugin editor",
                                 new BMessage(MSG_FX_TOGGLE_UI)))
                .AddGlue()
            .End();
        // clang-format on
    }

    guint n = pluginhost_param_count(inst);
    if (n == 0)
        builder.Add(new BStringView("none", "This plugin exposes no editable parameters."));

    for (guint i = 0; i < n; i++) {
        char name[128];
        pluginhost_param_name(inst, i, name, sizeof(name));

        BMessage *msg = new BMessage(MSG_FX_PARAM);
        msg->AddInt32("param", (int32)i);
        BSlider *slider = new BSlider("param", name, msg, 0, kSliderSteps, B_HORIZONTAL);
        BMessage *mod = new BMessage(MSG_FX_PARAM);
        mod->AddInt32("param", (int32)i);
        slider->SetModificationMessage(mod);
        slider->SetValue((int32)(pluginhost_param_get(inst, i) * kSliderSteps));
        // A usable throw length without manual window resizing.
        slider->SetExplicitMinSize(BSize(320.0f, B_SIZE_UNSET));

        BStringView *value = new BStringView("value", "");
        value->SetExplicitMinSize(BSize(110.0f, B_SIZE_UNSET));

        // clang-format off
        builder
            .AddGroup(B_HORIZONTAL, 6.0f)
                .Add(slider)
                .Add(value)
            .End();
        // clang-format on

        m_sliders.push_back(slider);
        m_value_labels.push_back(value);
        UpdateValueLabel(i);
    }
    // Trailing glue keeps the parameters packed at the top; the scroll view
    // handles any overflow, so the window frame never has to change.
    builder.AddGlue();
}

// A drum rack: one row per visible slot — [ Load… | file | Volume | Learn |
// Note ] — plus a +Add slot button. Slot volume/note are ordinary VST3
// parameters (reached by slot index through the ph_drum_* API); the sample path
// travels over the IDrumLoader extension; Learn is captured host-side.
void FxWindow::BuildDrumRack(PluginInstance *inst)
{
    // Fresh, consistent learn state whenever the rack is (re)built.
    ph_drum_learn_arm(inst, false);

    int maxSlots = (int)ph_drum_max_slots();
    int count = (int)ph_drum_slot_count(inst);
    if (count < 1)
        count = 1;
    if (count > maxSlots)
        count = maxSlots;

    BLayoutBuilder::Group<> builder(m_param_group->GroupLayout());

    for (int i = 0; i < count; i++) {
        BMessage *loadMsg = new BMessage(MSG_DRUM_LOAD);
        loadMsg->AddInt32("slot", i);
        BButton *load = new BButton("load", "Load…", loadMsg);

        char path[1024], label[1100];
        ph_drum_file_get(inst, i, path, sizeof(path));
        if (path[0])
            fx_basename(path, "", label, sizeof(label));
        else
            snprintf(label, sizeof(label), "(empty)");
        BStringView *file = new BStringView("file", label);
        file->SetExplicitMinSize(BSize(150.0f, B_SIZE_UNSET));

        BMessage *volMsg = new BMessage(MSG_DRUM_VOL);
        volMsg->AddInt32("slot", i);
        BSlider *vol = new BSlider("vol", NULL, volMsg, 0, kSliderSteps, B_HORIZONTAL);
        BMessage *volMod = new BMessage(MSG_DRUM_VOL);
        volMod->AddInt32("slot", i);
        vol->SetModificationMessage(volMod);
        vol->SetValue((int32)(ph_drum_volume_get(inst, i) * kSliderSteps));
        vol->SetExplicitMinSize(BSize(160.0f, B_SIZE_UNSET));

        BMessage *learnMsg = new BMessage(MSG_DRUM_LEARN);
        learnMsg->AddInt32("slot", i);
        BCheckBox *learn = new BCheckBox("learn", "Learn", learnMsg);

        gint note = ph_drum_note_get(inst, i);
        char noteLabel[32];
        if (note < 0)
            snprintf(noteLabel, sizeof(noteLabel), "Note: —");
        else
            snprintf(noteLabel, sizeof(noteLabel), "Note: %d", (int)note);
        BStringView *noteView = new BStringView("note", noteLabel);
        noteView->SetExplicitMinSize(BSize(80.0f, B_SIZE_UNSET));

        // clang-format off
        builder
            .AddGroup(B_HORIZONTAL, 6.0f)
                .Add(load)
                .Add(file)
                .Add(vol)
                .Add(learn)
                .Add(noteView)
            .End();
        // clang-format on

        m_drum_file.push_back(file);
        m_drum_vol.push_back(vol);
        m_drum_learn.push_back(learn);
        m_drum_note.push_back(noteView);
    }

    BButton *add = new BButton("add-slot", "+ Add slot", new BMessage(MSG_DRUM_ADD));
    // clang-format off
    builder
        .AddGroup(B_HORIZONTAL, 6.0f)
            .Add(add)
            .AddGlue()
        .End();
    // clang-format on
    builder.AddGlue();
}

void FxWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_FX_RAISE:
            Activate();
            break;

        case MSG_FX_SYNC:
            // SetValue does not invoke, so this cannot echo back as a click.
            SyncControlsRow();
            break;

        case MSG_FX_ADD: {
            const char *key = NULL, *name = NULL, *category = NULL;
            int32 format = PH_VST3;
            message->FindString("key", &key);
            message->FindString("name", &name);
            message->FindString("category", &category);
            message->FindInt32("format", &format);
            if (!key || !name || format < 0 || format >= PH_NFORMATS)
                break;
            PluginInfo info;
            info.format = (PluginFormat)format;
            info.key = (char *)key;
            info.name = (char *)name;
            info.category = (char *)(category ? category : "");
            info.is_instrument = FALSE; /* derived from the category on load */
            PluginInstance *inst = pluginhost_instantiate(&info);
            if (!inst) {
                g_warning("FxWindow: could not instantiate '%s'", name);
                char text[256];
                snprintf(text, sizeof(text), "Could not load the plugin \"%s\".", name);
                (new BAlert("fx-load-failed", text, "OK", NULL, NULL, B_WIDTH_AS_USUAL,
                            B_WARNING_ALERT))
                    ->Go(NULL);
                break;
            }
            jackdaw_track_fx_add(m_track, inst);
            RebuildChainList((int)jackdaw_track_fx_count(m_track) - 1);
            NotifyChainChanged();
            break;
        }

        case MSG_FX_SELECT:
            RebuildParamPanel();
            break;

        case MSG_FXUI_TICK: {
            // We are the looper thread (locked in MessageReceived), as the
            // HaikuUI port_event contract requires.
            if (m_embedded_inst)
                pluginhost_ui_poll(m_embedded_inst);
            break;
        }

        case PH_MSG_EDITOR_RESIZED: {
            // A plug-in resized its own editor frame (posted by the VST2/VST3/LV2
            // backend). All embedded-view sizing happens here on the looper
            // thread. Optional width/height set the new editor size; otherwise
            // the backend already updated the view and we just re-fit the window.
            if (!m_embedded_ui)
                break;
            float w, h;
            if (message->FindFloat("width", &w) == B_OK &&
                message->FindFloat("height", &h) == B_OK && w > 0.0f && h > 0.0f) {
                m_embedded_ui->SetExplicitMinSize(BSize(w, h));
                m_embedded_ui->SetExplicitMaxSize(BSize(w, h));
                m_embedded_ui->SetExplicitPreferredSize(BSize(w, h));
            }
            FitWindowToEmbeddedEditor();
            break;
        }

        case MSG_FX_TOGGLE_UI: {
            PluginInstance *inst = Selected();
            if (!inst)
                break;
            if (!m_prefer_generic.erase(inst))
                m_prefer_generic.insert(inst);
            RebuildParamPanel();
            break;
        }

        case MSG_FX_REMOVE: {
            int32 sel = m_chain_list->CurrentSelection();
            if (sel < 0)
                break;
            // The editor must be torn down before its instance is retired.
            PluginInstance *inst = (PluginInstance *)jackdaw_track_fx_get(m_track, (guint)sel);
            if (inst && inst == m_embedded_inst)
                DetachEmbeddedUi(true);
            else if (inst)
                pluginhost_ui_destroy(inst);
            if (inst)
                m_prefer_generic.erase(inst); // pointer may be reused later
            jackdaw_track_fx_remove(m_track, (guint)sel);
            ArmFxCollect();
            guint n = jackdaw_track_fx_count(m_track);
            RebuildChainList(n > 0 ? MIN((int)n - 1, (int)sel) : -1);
            NotifyChainChanged();
            break;
        }

        case MSG_FX_COLLECT:
            // The retired instance is freed here rather than on the next chain
            // edit: a Wine-bridged plug-in keeps its Wine host process alive
            // until pluginhost_free reaches its effClose / module release.
            // Runs on this looper, which is also the thread that instantiated
            // it — the same-thread contract pluginhost.h requires.
            if (!jackdaw_track_fx_collect(m_track))
                ArmFxCollect(); // RT has not cycled yet; try again shortly
            break;

        case MSG_FX_UP:
        case MSG_FX_DOWN: {
            int32 sel = m_chain_list->CurrentSelection();
            if (sel < 0)
                break;
            int to = sel + (message->what == MSG_FX_UP ? -1 : 1);
            if (to < 0 || to >= (int)jackdaw_track_fx_count(m_track))
                break;
            jackdaw_track_fx_move(m_track, (guint)sel, (guint)to);
            RebuildChainList(to);
            break;
        }

        case MSG_FX_BYPASS: {
            PluginInstance *inst = Selected();
            if (!inst)
                break;
            pluginhost_set_active(inst, m_bypass->Value() != B_CONTROL_ON);
            RebuildChainList(m_chain_list->CurrentSelection());
            break;
        }

        case MSG_FX_MIX: {
            PluginInstance *inst = Selected();
            if (inst)
                pluginhost_set_mix(inst, (float)m_mix->Value() / kSliderSteps);
            break;
        }

        case MSG_FX_PARAM: {
            PluginInstance *inst = Selected();
            int32 param = -1;
            if (!inst || message->FindInt32("param", &param) != B_OK)
                break;
            if (param < 0 || (size_t)param >= m_sliders.size())
                break;
            float v = (float)m_sliders[param]->Value() / kSliderSteps;
            pluginhost_param_set(inst, (guint)param, v);
            UpdateValueLabel((guint)param);
            break;
        }

        case MSG_DRUM_LOAD: {
            int32 slot = -1;
            if (message->FindInt32("slot", &slot) != B_OK)
                break;
            if (!m_file_panel)
                m_file_panel = new BFilePanel(B_OPEN_PANEL);
            BMessage tmpl(MSG_DRUM_FILE_REFS);
            tmpl.AddInt32("slot", slot);
            m_file_panel->SetTarget(BMessenger(this));
            m_file_panel->SetMessage(&tmpl);
            m_file_panel->Window()->SetTitle("Load drum sample (.wav)");
            m_file_panel->Show();
            break;
        }

        case MSG_DRUM_FILE_REFS: {
            PluginInstance *inst = Selected();
            entry_ref ref;
            int32 slot = -1;
            message->FindInt32("slot", &slot);
            if (!inst || slot < 0 || message->FindRef("refs", &ref) != B_OK)
                break;
            BPath path(&ref);
            if (path.InitCheck() != B_OK)
                break;
            if (!ph_drum_file_set(inst, (int)slot, path.Path()))
                g_warning("FxWindow: drum plugin refused '%s'", path.Path());
            if (slot < (int32)m_drum_file.size()) {
                char label[1100];
                fx_basename(path.Path(), "", label, sizeof(label));
                m_drum_file[slot]->SetText(label);
            }
            break;
        }

        case MSG_DRUM_VOL: {
            PluginInstance *inst = Selected();
            int32 slot = -1;
            if (!inst || message->FindInt32("slot", &slot) != B_OK)
                break;
            if (slot < 0 || (size_t)slot >= m_drum_vol.size())
                break;
            float v = (float)m_drum_vol[slot]->Value() / kSliderSteps;
            ph_drum_volume_set(inst, (int)slot, v);
            break;
        }

        case MSG_DRUM_LEARN: {
            PluginInstance *inst = Selected();
            int32 slot = -1;
            if (!inst || message->FindInt32("slot", &slot) != B_OK)
                break;
            if (slot < 0 || (size_t)slot >= m_drum_learn.size())
                break;
            bool on = m_drum_learn[slot]->Value() == B_CONTROL_ON;
            if (on) {
                // Only one slot can be armed at a time.
                for (size_t k = 0; k < m_drum_learn.size(); k++)
                    if ((int32)k != slot)
                        m_drum_learn[k]->SetValue(B_CONTROL_OFF);
                m_learn_slot = slot;
                ph_drum_learn_arm(inst, true);
                if (!m_learn_poll) {
                    BMessage poll(MSG_DRUM_POLL);
                    m_learn_poll = new BMessageRunner(BMessenger(this), &poll, 80000);
                }
            } else if (m_learn_slot == slot) {
                ph_drum_learn_arm(inst, false);
                m_learn_slot = -1;
                delete m_learn_poll;
                m_learn_poll = NULL;
            }
            break;
        }

        case MSG_DRUM_ADD: {
            PluginInstance *inst = Selected();
            if (!inst)
                break;
            ph_drum_add_slot(inst);
            RebuildParamPanel();
            break;
        }

        case MSG_DRUM_POLL: {
            PluginInstance *inst = Selected();
            if (!inst || m_learn_slot < 0) {
                delete m_learn_poll;
                m_learn_poll = NULL;
                break;
            }
            gint note = ph_drum_learn_take_note(inst);
            if (note >= 0) {
                int slot = m_learn_slot;
                ph_drum_note_set(inst, slot, note);
                if (slot < (int)m_drum_note.size()) {
                    char noteLabel[32];
                    snprintf(noteLabel, sizeof(noteLabel), "Note: %d", (int)note);
                    m_drum_note[slot]->SetText(noteLabel);
                }
                if (slot < (int)m_drum_learn.size())
                    m_drum_learn[slot]->SetValue(B_CONTROL_OFF);
                ph_drum_learn_arm(inst, false);
                m_learn_slot = -1;
                delete m_learn_poll;
                m_learn_poll = NULL;
            }
            break;
        }

        case MSG_PRESET_SAVE: {
            PluginInstance *inst = Selected();
            if (!inst)
                break;
            if (!m_save_panel)
                m_save_panel = new BFilePanel(B_SAVE_PANEL);
            BMessage tmpl(MSG_PRESET_SAVE_REFS);
            m_save_panel->SetTarget(BMessenger(this));
            m_save_panel->SetMessage(&tmpl);
            char def[256];
            snprintf(def, sizeof(def), "%s.jdpreset", pluginhost_name(inst));
            m_save_panel->SetSaveText(def);
            m_save_panel->Window()->SetTitle("Save preset (.jdpreset)");
            m_save_panel->Show();
            break;
        }

        case MSG_PRESET_LOAD: {
            PluginInstance *inst = Selected();
            if (!inst)
                break;
            if (!m_file_panel)
                m_file_panel = new BFilePanel(B_OPEN_PANEL);
            BMessage tmpl(MSG_PRESET_LOAD_REFS);
            m_file_panel->SetTarget(BMessenger(this));
            m_file_panel->SetMessage(&tmpl);
            m_file_panel->Window()->SetTitle("Load preset (.jdpreset)");
            m_file_panel->Show();
            break;
        }

        case MSG_PRESET_SAVE_REFS: {
            PluginInstance *inst = Selected();
            entry_ref dir;
            const char *name = NULL;
            if (!inst || message->FindRef("directory", &dir) != B_OK ||
                message->FindString("name", &name) != B_OK || !name)
                break;
            BPath path(&dir);
            if (path.InitCheck() != B_OK || path.Append(name) != B_OK)
                break;
            if (!pluginhost_preset_save(inst, path.Path()))
                g_warning("FxWindow: preset save failed for '%s'", path.Path());
            break;
        }

        case MSG_PRESET_LOAD_REFS: {
            PluginInstance *inst = Selected();
            entry_ref ref;
            if (!inst || message->FindRef("refs", &ref) != B_OK)
                break;
            BPath path(&ref);
            if (path.InitCheck() != B_OK)
                break;
            if (!pluginhost_preset_load(inst, path.Path())) {
                g_warning("FxWindow: preset load failed for '%s'", path.Path());
                break;
            }
            // Restored state can change slot count, sample names, notes, volumes,
            // and (for other plugins) parameter titles — all read only at panel
            // build time, so rebuild the whole panel to reflect the loaded kit.
            RebuildParamPanel();
            break;
        }

        default:
            BWindow::MessageReceived(message);
            break;
    }
}
