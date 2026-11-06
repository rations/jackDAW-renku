#pragma once

#include <Invoker.h>
#include <String.h>
#include <View.h>

#include <vector>

// A pop-up selector for a port name (a track's input) that keeps a fixed
// width. BMenuField grows to fit its marked label, so a long Media Kit name
// ("UMC204HD 192k / input 0 / 1", "MediaPlayer audio out #2 / 1") widened the
// whole track strip and pushed the VU meter out of view. This view takes the
// width the layout gives it and shortens the name to fit: the device part
// first ("UMC2… / input 0 / 1"), then the middle of the rest. The full name
// is in the tool tip.
//
// The pop-up is built when it opens: "None", then one item per port, grouped
// in a submenu per device (the part before the first " / " of a Media Kit
// name, the client of a JACK "client:port") when a device has several. A
// chosen item Invokes a copy of the message with a "port" string ("" = none).
class PortSelector : public BView, public BInvoker
{
public:
    PortSelector(const char *name, BMessage *message);

    void AttachedToWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *drag) override;

    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;

    // The ports the pop-up offers (full names, as the engine lists them).
    void SetChoices(const char *const *ports);
    // The selected port's full name; NULL or "" is none.
    void SetSelected(const char *port);

private:
    float FieldHeight();
    void UpdateToolTip();
    BMessage *ItemMessage(const BString &port) const;

    std::vector<BString> m_choices;
    BString m_selected;
    bool m_hover;
};
