#include "PortSelector.h"

#include <ControlLook.h>
#include <Font.h>
#include <LayoutUtils.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Message.h>
#include <PopUpMenu.h>

#include <math.h>
#include <string.h>

// The Media Kit backend's audio and MIDI port names start with these.
static const char *const kBackendPrefixes[] = {"media:", "midi:"};
static const char *kEllipsis = "\xE2\x80\xA6";
static const float kMinWidth = 48.0f;
static const float kPopUpIndicatorWidth = 13.0f; // as BMenuField draws it

// Split a port's full name into the device it belongs to and the port on that
// device, plus the separator to show between them. "media:UMC204HD 192k /
// input 0 / 1" -> "UMC204HD 192k", " / ", "input 0 / 1"; JACK's
// "system:capture_1" -> "system", ":", "capture_1". A name with no device part
// ("midi:/dev/midi/usb/0-0") comes back as the port alone.
static void split_port(const char *full, BString &device, BString &sep, BString &port)
{
    device = "";
    port = "";
    sep = "";
    if (!full)
        return;
    for (const char *prefix : kBackendPrefixes) {
        size_t plen = strlen(prefix);
        if (strncmp(full, prefix, plen) != 0)
            continue;
        const char *name = full + plen;
        const char *slash = strstr(name, " / ");
        if (slash && slash != name && slash[3]) {
            device.SetTo(name, (int32)(slash - name));
            sep = " / ";
            port = slash + 3;
        } else {
            port = name;
        }
        return;
    }
    const char *colon = strchr(full, ':');
    if (colon && colon != full && colon[1]) {
        device.SetTo(full, (int32)(colon - full));
        sep = ":";
        port = colon + 1;
    } else {
        port = full;
    }
}

// The whole name as the user reads it (no backend prefix).
static BString display_name(const char *full)
{
    BString device, sep, port;
    split_port(full, device, sep, port);
    BString s(device);
    s << sep << port;
    return s;
}

// Fit a port's name into `width`. The port part is what tells two channels of
// one card apart, so the device name is shortened first and the port part
// only when there is no room left for a few letters of the device.
static BString fit_name(const BFont &font, const char *full, float width)
{
    BString device, sep, port;
    split_port(full, device, sep, port);
    BString s(device);
    s << sep << port;
    if (font.StringWidth(s.String()) <= width)
        return s;
    if (device.Length() > 0) {
        BString tail(sep);
        tail << port;
        float room = width - font.StringWidth(tail.String());
        BString least("XX");
        least << kEllipsis;
        if (room >= font.StringWidth(least.String())) {
            font.TruncateString(&device, B_TRUNCATE_END, room);
            device << tail;
            return device;
        }
    }
    font.TruncateString(&port, B_TRUNCATE_MIDDLE, width);
    return port;
}

PortSelector::PortSelector(const char *name, BMessage *message)
    : BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE), BInvoker(message, NULL), m_hover(false)
{
    UpdateToolTip();
}

void PortSelector::AttachedToWindow()
{
    BView::AttachedToWindow();
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}

float PortSelector::FieldHeight()
{
    font_height fh;
    GetFontHeight(&fh);
    // Text plus the frame and inner margins, the height BMenuField has for
    // the same font.
    return ceilf(fh.ascent) + ceilf(fh.descent) + 8.0f;
}

// Width is not taken from the label: the strip decides it, and the label
// is fitted to that. Only the height follows the font.
BSize PortSelector::MinSize()
{
    return BLayoutUtils::ComposeSize(ExplicitMinSize(), BSize(kMinWidth, FieldHeight()));
}

BSize PortSelector::MaxSize()
{
    return BLayoutUtils::ComposeSize(ExplicitMaxSize(), BSize(B_SIZE_UNLIMITED, FieldHeight()));
}

BSize PortSelector::PreferredSize()
{
    return BLayoutUtils::ComposeSize(ExplicitPreferredSize(), BSize(kMinWidth, FieldHeight()));
}

void PortSelector::Draw(BRect updateRect)
{
    BRect rect(Bounds());
    uint32 flags = 0;
    if (m_hover)
        flags |= BControlLook::B_HOVER;
    rgb_color base = ui_color(B_CONTROL_BACKGROUND_COLOR);
    be_control_look->DrawMenuFieldFrame(this, rect, updateRect, base, ViewColor(), flags);
    be_control_look->DrawMenuFieldBackground(this, rect, updateRect, base, true, flags);

    float spacing = be_control_look->DefaultLabelSpacing();
    BRect text(rect);
    text.left += spacing;
    text.right -= kPopUpIndicatorWidth + 2.0f;
    if (!text.IsValid())
        return;

    BFont font;
    GetFont(&font);
    BString label = m_selected.Length() > 0 ? fit_name(font, m_selected.String(), text.Width())
                                            : BString("None");
    font_height fh;
    font.GetHeight(&fh);
    float y = floorf(text.top + (text.Height() - (fh.ascent + fh.descent)) / 2.0f + fh.ascent);
    SetHighUIColor(B_CONTROL_TEXT_COLOR);
    SetLowColor(base);
    DrawString(label.String(), BPoint(text.left, y));
}

void PortSelector::MouseMoved(BPoint where, uint32 code, const BMessage *drag)
{
    (void)drag;
    bool inside = code != B_EXITED_VIEW && Bounds().Contains(where);
    if (inside != m_hover) {
        m_hover = inside;
        Invalidate();
    }
}

BMessage *PortSelector::ItemMessage(const BString &port) const
{
    BMessage *m = new BMessage(*Message());
    m->AddString("port", port.String());
    return m;
}

void PortSelector::MouseDown(BPoint where)
{
    (void)where;
    if (!Message())
        return;

    BPopUpMenu *menu = new BPopUpMenu("ports", false, false);
    menu->SetFont(be_plain_font);

    BMenuItem *none = new BMenuItem("None", ItemMessage(BString("")));
    none->SetMarked(m_selected.Length() == 0);
    menu->AddItem(none);

    // The selected port is no longer offered (the device went away): show it,
    // marked and greyed, so the menu says what the track still asks for.
    bool listed = m_selected.Length() == 0;
    for (size_t i = 0; i < m_choices.size() && !listed; i++)
        listed = m_choices[i] == m_selected;
    if (!listed) {
        BString label = display_name(m_selected.String());
        label << " (not present)";
        BMenuItem *gone = new BMenuItem(label.String(), NULL);
        gone->SetMarked(true);
        gone->SetEnabled(false);
        menu->AddItem(gone);
    }
    if (!m_choices.empty())
        menu->AddSeparatorItem();

    // One group per device, in the order the devices are first listed.
    std::vector<bool> done(m_choices.size(), false);
    for (size_t i = 0; i < m_choices.size(); i++) {
        if (done[i])
            continue;
        BString device, sep, port;
        split_port(m_choices[i].String(), device, sep, port);
        std::vector<size_t> group;
        for (size_t j = i; j < m_choices.size(); j++) {
            if (done[j])
                continue;
            BString d, s, p;
            split_port(m_choices[j].String(), d, s, p);
            if (d == device) {
                group.push_back(j);
                done[j] = true;
            }
        }
        if (device.Length() == 0 || group.size() == 1) {
            for (size_t k = 0; k < group.size(); k++) {
                const BString &full = m_choices[group[k]];
                BMenuItem *it = new BMenuItem(display_name(full.String()).String(),
                                              ItemMessage(full));
                it->SetMarked(full == m_selected);
                menu->AddItem(it);
            }
            continue;
        }
        BMenu *sub = new BMenu(device.String());
        sub->SetFont(be_plain_font);
        bool marked = false;
        for (size_t k = 0; k < group.size(); k++) {
            const BString &full = m_choices[group[k]];
            BString d, s, p;
            split_port(full.String(), d, s, p);
            BMenuItem *it = new BMenuItem(p.String(), ItemMessage(full));
            it->SetMarked(full == m_selected);
            marked = marked || full == m_selected;
            sub->AddItem(it);
        }
        menu->AddItem(sub);
        if (marked && sub->Superitem())
            sub->Superitem()->SetMarked(true);
    }

    menu->SetTargetForItems(Messenger());
    for (int32 i = 0; i < menu->CountItems(); i++)
        if (BMenu *sub = menu->SubmenuAt(i))
            sub->SetTargetForItems(Messenger());

    BRect frame = ConvertToScreen(Bounds());
    menu->SetAsyncAutoDestruct(true);
    menu->Go(frame.LeftBottom() + BPoint(0.0f, 1.0f), true, true, frame, true);
}

void PortSelector::SetChoices(const char *const *ports)
{
    m_choices.clear();
    for (int i = 0; ports && ports[i]; i++)
        m_choices.push_back(BString(ports[i]));
}

void PortSelector::SetSelected(const char *port)
{
    BString sel(port ? port : "");
    if (sel == m_selected)
        return;
    m_selected = sel;
    UpdateToolTip();
    Invalidate();
}

void PortSelector::UpdateToolTip()
{
    BString tip("Input: ");
    if (m_selected.Length() > 0)
        tip << display_name(m_selected.String());
    else
        tip << "None";
    SetToolTip(tip.String());
}
