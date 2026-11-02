#include "MidiControlWindow.h"

#include <Button.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>

#include <cstdio>

#include "Messages.h"
#include "StepperControl.h"

// Internal: any per-row field control changed; the handler re-reads that whole
// row and posts a complete mapping to MainWindow.
enum {
    MSG_ROW_CHANGED = 'mrch',
};

// Action labels, indexed by MidiCtlAction.
static const char *kActionLabels[ACT_NACTIONS] = {
    "Toggle FX bypass", "Momentary bypass", "Set FX mix",     "Set FX param",     "Toggle mute",
    "Switch group",     "Transport play",   "Transport stop", "Transport record",
};

// Short human label for a mapping's learned trigger (or the learn prompt).
static void trigger_label(const MidiCtlMapping &m, bool learning, char *buf, size_t len)
{
    if (learning) {
        snprintf(buf, len, "Learning…");
        return;
    }
    if (m.msg_type == MIDI_CTL_UNLEARNED) {
        snprintf(buf, len, "Learn");
        return;
    }
    const char *ty = "?";
    bool has_num = true;
    switch (m.msg_type) {
        case MIDI_CTL_CC:
            ty = "CC";
            break;
        case MIDI_CTL_NOTE:
            ty = "Note";
            break;
        case MIDI_CTL_PROGRAM:
            ty = "Prog";
            break;
        case MIDI_CTL_PITCHBEND:
            ty = "Bend";
            has_num = false;
            break;
        default:
            break;
    }
    char ch[16];
    if (m.channel < 0)
        snprintf(ch, sizeof ch, "any");
    else
        snprintf(ch, sizeof ch, "%d", m.channel + 1);
    if (has_num)
        snprintf(buf, len, "%s %u · ch %s", ty, m.number, ch);
    else
        snprintf(buf, len, "%s · ch %s", ty, ch);
}

MidiControlWindow::MidiControlWindow(BMessenger target)
    : BWindow(BRect(160, 160, 760, 460), "MIDI Control", B_TITLED_WINDOW,
              B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
      m_target(target), m_cur_source(""), m_learn_index(-1), m_source(NULL), m_rows_box(NULL)
{
    m_source = new BMenuField("source", "Control input:", new BPopUpMenu("(none)"));

    BButton *add = new BButton("add", "Add mapping", new BMessage(MSG_MIDICTL_ADD));
    add->SetTarget(this);

    m_rows_box = new BGroupView(B_VERTICAL, 4.0f);

    BScrollView *scroll =
        new BScrollView("rows-scroll", m_rows_box, 0, false, true, B_FANCY_BORDER);

    BStringView *hint = new BStringView(
        "hint", "Connect a footswitch / controller to jackdaw:control_in, then Learn a row.");
    // The effect plug-ins hear the same device (pluginhost_process_fx_midi).
    BStringView *hint2 =
        new BStringView("hint2", "Effect plug-ins with MIDI features of their own hear it too.");

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
        .Add(m_source)
        .AddGlue()
        .Add(add)
        .End()
        .Add(scroll)
        .Add(hint)
        .Add(hint2);

    RebuildRows();
}

BMenuField *MidiControlWindow::BuildActionMenu(int index, uint8 action)
{
    BPopUpMenu *menu = new BPopUpMenu("action");
    for (int a = 0; a < ACT_NACTIONS; a++) {
        BMessage *msg = new BMessage(MSG_ROW_CHANGED);
        msg->AddInt32("index", index);
        BMenuItem *it = new BMenuItem(kActionLabels[a], msg);
        it->SetTarget(this);
        if (a == (int)action)
            it->SetMarked(true);
        menu->AddItem(it);
    }
    return new BMenuField("action", NULL, menu);
}

BMenuField *MidiControlWindow::BuildTrackMenu(int index, int32 track)
{
    BPopUpMenu *menu = new BPopUpMenu("track");
    BMessage *none = new BMessage(MSG_ROW_CHANGED);
    none->AddInt32("index", index);
    BMenuItem *none_it = new BMenuItem("— (no track)", none);
    none_it->SetTarget(this);
    if (track < 0)
        none_it->SetMarked(true);
    menu->AddItem(none_it);

    for (size_t t = 0; t < m_track_names.size(); t++) {
        BMessage *msg = new BMessage(MSG_ROW_CHANGED);
        msg->AddInt32("index", index);
        BMenuItem *it = new BMenuItem(m_track_names[t].String(), msg);
        it->SetTarget(this);
        if ((int32)t == track)
            it->SetMarked(true);
        menu->AddItem(it);
    }
    return new BMenuField("track", NULL, menu);
}

void MidiControlWindow::RebuildRows()
{
    // Tear down the old rows (views + layout items), mirroring the FX param panel.
    BView *child;
    while ((child = m_rows_box->ChildAt(0)) != NULL) {
        m_rows_box->RemoveChild(child);
        delete child;
    }
    BLayout *layout = m_rows_box->GroupLayout();
    while (layout->CountItems() > 0)
        delete layout->RemoveItem((int32)0);
    m_rows.clear();

    BLayoutBuilder::Group<> builder(m_rows_box->GroupLayout());

    if (m_maps.empty()) {
        builder.Add(new BStringView("empty", "No mappings. Click “Add mapping”."));
        builder.AddGlue();
        return;
    }

    for (size_t i = 0; i < m_maps.size(); i++) {
        const MidiCtlMapping &m = m_maps[i];
        Row row;
        row.map = m;

        char trig[48];
        trigger_label(m, m_learn_index == (int)i, trig, sizeof trig);
        BMessage *learn_msg = new BMessage(MSG_MIDICTL_LEARN);
        learn_msg->AddInt32("index", (int32)i);
        row.learn = new BButton("learn", trig, learn_msg);
        row.learn->SetTarget(this);

        row.action = BuildActionMenu((int)i, m.action);
        row.track = BuildTrackMenu((int)i, m.track_index);

        BMessage *fx_msg = new BMessage(MSG_ROW_CHANGED);
        fx_msg->AddInt32("index", (int32)i);
        row.fx = new StepperControl("fx", "FX", fx_msg, -1, 63, 1, 0);
        row.fx->SetValue(m.fx_index);
        row.fx->SetTarget(this);

        BMessage *param_msg = new BMessage(MSG_ROW_CHANGED);
        param_msg->AddInt32("index", (int32)i);
        row.param = new StepperControl("param", "Param", param_msg, -1, 4095, 1, 0);
        row.param->SetValue(m.param_index);
        row.param->SetTarget(this);

        BMessage *group_msg = new BMessage(MSG_ROW_CHANGED);
        group_msg->AddInt32("index", (int32)i);
        row.group = new StepperControl("group", "Grp", group_msg, -1, 63, 1, 0);
        row.group->SetValue(m.switch_group);
        row.group->SetTarget(this);
        row.group->SetToolTip("Like a pedalboard: switches with the same group number turn each "
                              "other's tracks off, and pressing the one that is on turns it off. "
                              "Tracks without a switch in the group keep playing.");

        BMessage *rm_msg = new BMessage(MSG_MIDICTL_REMOVE);
        rm_msg->AddInt32("index", (int32)i);
        row.remove = new BButton("remove", "Remove", rm_msg);
        row.remove->SetTarget(this);

        m_rows.push_back(row);

        // clang-format off
        builder
            .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
                .Add(row.learn)
                .Add(row.action)
                .Add(row.track)
                .Add(row.fx)
                .Add(row.param)
                .Add(row.group)
                .Add(row.remove)
            .End();
        // clang-format on
    }
    builder.AddGlue();
}

void MidiControlWindow::PostRow(int index)
{
    if (index < 0 || index >= (int)m_rows.size())
        return;
    Row &row = m_rows[index];

    int32 action = 0;
    if (BMenuItem *mk = row.action->Menu()->FindMarked())
        action = row.action->Menu()->IndexOf(mk);

    int32 track = -1;
    if (BMenuItem *mk = row.track->Menu()->FindMarked()) {
        int32 idx = row.track->Menu()->IndexOf(mk);
        track = idx <= 0 ? -1 : idx - 1; // item 0 is "no track"
    }

    BMessage out(MSG_MIDICTL_UPDATE);
    out.AddInt32("index", index);
    // Trigger fields are unchanged by these controls (set via Learn).
    out.AddInt32("msg_type", row.map.msg_type);
    out.AddInt32("channel", row.map.channel);
    out.AddInt32("number", row.map.number);
    out.AddInt32("action", action);
    out.AddInt32("track", track);
    out.AddInt32("fx", (int32)(row.fx->Value() + (row.fx->Value() < 0 ? -0.5 : 0.5)));
    out.AddInt32("param", (int32)(row.param->Value() + (row.param->Value() < 0 ? -0.5 : 0.5)));
    out.AddInt32("group", (int32)(row.group->Value() + (row.group->Value() < 0 ? -0.5 : 0.5)));
    m_target.SendMessage(&out);
}

void MidiControlWindow::ApplySnapshot(const BMessage *snap)
{
    m_maps.clear();
    m_track_names.clear();
    m_ports.clear();

    const void *data = NULL;
    ssize_t sz = 0;
    for (int32 i = 0; snap->FindData("map", B_RAW_TYPE, i, &data, &sz) == B_OK; i++) {
        if (sz == (ssize_t)sizeof(MidiCtlMapping))
            m_maps.push_back(*(const MidiCtlMapping *)data);
    }
    BString s;
    for (int32 i = 0; snap->FindString("track", i, &s) == B_OK; i++)
        m_track_names.push_back(s);
    for (int32 i = 0; snap->FindString("port", i, &s) == B_OK; i++)
        m_ports.push_back(s);
    if (snap->FindString("cursource", &s) == B_OK)
        m_cur_source = s;
    else
        m_cur_source = "";
    int32 learn = -1;
    snap->FindInt32("learn", &learn);
    m_learn_index = learn;

    // Rebuild the control-input source menu.
    BMenu *menu = m_source->Menu();
    while (menu->CountItems() > 0)
        delete menu->RemoveItem((int32)0);
    BMessage *none = new BMessage(MSG_MIDICTL_SOURCE);
    none->AddString("port", "");
    BMenuItem *none_it = new BMenuItem("(none)", none);
    none_it->SetTarget(this);
    if (m_cur_source.Length() == 0)
        none_it->SetMarked(true);
    menu->AddItem(none_it);
    for (size_t p = 0; p < m_ports.size(); p++) {
        BMessage *msg = new BMessage(MSG_MIDICTL_SOURCE);
        msg->AddString("port", m_ports[p].String());
        BMenuItem *it = new BMenuItem(m_ports[p].String(), msg);
        it->SetTarget(this);
        if (m_cur_source == m_ports[p])
            it->SetMarked(true);
        menu->AddItem(it);
    }

    RebuildRows();
}

void MidiControlWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_MIDICTL_SNAPSHOT:
            ApplySnapshot(message);
            break;
        case MSG_ROW_CHANGED: {
            int32 index = -1;
            message->FindInt32("index", &index);
            PostRow(index);
            break;
        }
        // Learn / Remove / Source / Add already carry the right `what` and
        // payload for MainWindow; relay them unchanged.
        case MSG_MIDICTL_LEARN:
        case MSG_MIDICTL_REMOVE:
        case MSG_MIDICTL_SOURCE:
        case MSG_MIDICTL_ADD:
            m_target.SendMessage(message);
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool MidiControlWindow::QuitRequested()
{
    Hide();
    return false; // keep the singleton alive; MainWindow re-shows it
}
