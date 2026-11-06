#include "PortSelector.h"

#include <ControlLook.h>
#include <Font.h>
#include <LayoutUtils.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Message.h>
#include <PopUpMenu.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
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

// A port name's trailing channel number: "input 0 / 2" -> "input 0 / ", 2;
// "capture_1" -> "capture_", 1; "1" -> "", 1. False if it has none.
static bool split_channel(const BString &port, BString &base, long &channel)
{
    const char *s = port.String();
    int32 len = port.Length();
    int32 i = len;
    while (i > 0 && isdigit((unsigned char)s[i - 1]))
        i--;
    if (i == len || len - i > 4)
        return false;
    channel = strtol(s + i, NULL, 10);
    base.SetTo(s, i);
    return true;
}

// The two ports' common name with the channels joined, "input 0 / 1+2", if
// they are a stereo pair: one device, the same name but for a trailing
// channel number n (odd) and n + 1. Their device and separator come back too.
static bool pair_name(const char *left, const char *right, BString &device, BString &sep,
                      BString &joined)
{
    BString rd, rs, lp, rp, lb, rb;
    split_port(left, device, sep, lp);
    split_port(right, rd, rs, rp);
    long lc = 0, rc = 0;
    if (rd != device || rs != sep || !split_channel(lp, lb, lc) || !split_channel(rp, rb, rc))
        return false;
    if (lb != rb || lc % 2 != 1 || rc != lc + 1)
        return false;
    joined = lb;
    joined << (int32)lc << "+" << (int32)rc;
    return true;
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

// Fit a device and port into `width`. The port part is what tells two
// channels of one card apart, so the device name is shortened first and the
// port part only when there is no room left for a few letters of the device.
static BString fit_parts(const BFont &font, BString device, const BString &sep, BString port,
                         float width)
{
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
    : BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE), BInvoker(message, NULL),
      m_offerPairs(false), m_hover(false)
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
    BString label("None");
    if (m_selected.Length() > 0) {
        BString device, sep, port;
        if (m_selectedRight.Length() > 0 &&
            pair_name(m_selected.String(), m_selectedRight.String(), device, sep, port)) {
            label = fit_parts(font, device, sep, port, text.Width());
        } else {
            split_port(m_selected.String(), device, sep, port);
            if (m_selectedRight.Length() > 0)
                port << " +"; // a right channel that is not this one's pair
            label = fit_parts(font, device, sep, port, text.Width());
        }
    }
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

BMessage *PortSelector::ItemMessage(const BString &port, const BString &portRight) const
{
    BMessage *m = new BMessage(*Message());
    m->AddString("port", port.String());
    if (portRight.Length() > 0)
        m->AddString("port_r", portRight.String());
    return m;
}

bool PortSelector::Offered(const BString &port) const
{
    for (size_t i = 0; i < m_choices.size(); i++)
        if (m_choices[i] == port)
            return true;
    return false;
}

void PortSelector::MouseDown(BPoint where)
{
    (void)where;
    if (!Message())
        return;

    BPopUpMenu *menu = new BPopUpMenu("ports", false, false);
    menu->SetFont(be_plain_font);

    const BString noPort("");
    BMenuItem *none = new BMenuItem("None", ItemMessage(noPort, noPort));
    none->SetMarked(m_selected.Length() == 0);
    menu->AddItem(none);

    // The selected source is no longer offered (the device went away): show
    // it, marked and greyed, so the menu says what the track still asks for.
    bool listed = m_selected.Length() == 0 ||
                  (Offered(m_selected) &&
                   (m_selectedRight.Length() == 0 || Offered(m_selectedRight)));
    if (!listed) {
        BString label = display_name(m_selected.String());
        if (m_selectedRight.Length() > 0)
            label << " + " << display_name(m_selectedRight.String());
        label << " (not present)";
        BMenuItem *gone = new BMenuItem(label.String(), NULL);
        gone->SetMarked(true);
        gone->SetEnabled(false);
        menu->AddItem(gone);
    }
    if (!m_choices.empty())
        menu->AddSeparatorItem();

    bool stereoSelected = m_selectedRight.Length() > 0;
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
                                              ItemMessage(full, noPort));
                it->SetMarked(!stereoSelected && full == m_selected);
                menu->AddItem(it);
            }
            continue;
        }
        BMenu *sub = new BMenu(device.String());
        sub->SetFont(be_plain_font);
        bool marked = false;

        // Stereo pairs first: a port and the next channel of the same name.
        int32 pairs = 0;
        if (m_offerPairs) {
            for (size_t a = 0; a < group.size(); a++) {
                for (size_t b = 0; b < group.size(); b++) {
                    const BString &left = m_choices[group[a]];
                    const BString &right = m_choices[group[b]];
                    BString d, s, joined;
                    if (a == b || !pair_name(left.String(), right.String(), d, s, joined))
                        continue;
                    BMenuItem *it = new BMenuItem(joined.String(), ItemMessage(left, right));
                    bool on = stereoSelected && left == m_selected && right == m_selectedRight;
                    it->SetMarked(on);
                    marked = marked || on;
                    sub->AddItem(it);
                    pairs++;
                }
            }
            if (pairs > 0)
                sub->AddSeparatorItem();
        }
        for (size_t k = 0; k < group.size(); k++) {
            const BString &full = m_choices[group[k]];
            BString d, s, p;
            split_port(full.String(), d, s, p);
            BMenuItem *it = new BMenuItem(p.String(), ItemMessage(full, noPort));
            bool on = !stereoSelected && full == m_selected;
            it->SetMarked(on);
            marked = marked || on;
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

void PortSelector::SetChoices(const char *const *ports, bool offerPairs)
{
    m_choices.clear();
    for (int i = 0; ports && ports[i]; i++)
        m_choices.push_back(BString(ports[i]));
    m_offerPairs = offerPairs;
}

void PortSelector::SetSelected(const char *port, const char *portRight)
{
    BString sel(port ? port : "");
    BString right(sel.Length() > 0 && portRight ? portRight : "");
    if (sel == m_selected && right == m_selectedRight)
        return;
    m_selected = sel;
    m_selectedRight = right;
    UpdateToolTip();
    Invalidate();
}

void PortSelector::UpdateToolTip()
{
    BString tip("Input: ");
    if (m_selected.Length() == 0) {
        tip << "None";
    } else if (m_selectedRight.Length() > 0) {
        tip << display_name(m_selected.String()) << " (L) + "
            << display_name(m_selectedRight.String()) << " (R)";
    } else {
        tip << display_name(m_selected.String());
    }
    SetToolTip(tip.String());
}
