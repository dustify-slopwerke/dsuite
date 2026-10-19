#include "PedalboardView.h"

#include "PluginProcessor.h"
#include "common/plugin/ParamLayout.h"
#include "common/ui/DustifyLookAndFeel.h"

namespace
{
constexpr int gap = 16, rowGap = 10, margin = 8;
constexpr int ampWidth = 92, ampHeight = 44;
constexpr int switchWidth = 36, foldWidth = 16; // on the right of a pedal's title strip
using P = DustifyLookAndFeel::Palette;

juce::Colour accentFor (damp::PedalId id)
{
    using enum damp::PedalId;
    switch (id)
    {
        case FaceFuzz: case BigFuzz: return juce::Colour (P::source);
        case Boost: case Compressor: case Wah: case Octave: case Pitch: case Eq: return juce::Colour (P::envelope);
        case Chorus: case Flanger: case Phaser: case Tremolo: case Vibrato: return juce::Colour (P::modulation);
        case Delay: case Reverb: return juce::Colour (P::effects);
        case MidDrive: case BluesDrive: case Distortion: case Rodent: case Count: break;
    }
    return juce::Colour (P::filter); // overdrives and distortions
}

// The disclosure triangle on a pedal's title strip (solid, unlike the board's signal chevrons):
// points right when folded, down when open
class FoldButton final : public juce::Button
{
public:
    FoldButton() : juce::Button ("fold") {}

    bool open = false;

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        juce::Path triangle;
        if (open)
            triangle.addTriangle (c.x - 4.0f, c.y - 2.0f, c.x + 4.0f, c.y - 2.0f, c.x, c.y + 3.0f);
        else
            triangle.addTriangle (c.x - 2.0f, c.y - 4.0f, c.x - 2.0f, c.y + 4.0f, c.x + 3.0f, c.y);
        g.setColour (highlighted ? juce::Colour (P::text) : juce::Colour (P::dimText));
        g.fillPath (triangle);
    }
};
} // namespace

// One tile: a pedal (its title strip with the switch, and its controls when open) or the amp,
// dragged by its title
class PedalboardView::Tile final : public juce::Component,
                                   public juce::SettableTooltipClient
{
public:
    Tile (DampAudioProcessor& p, int slotValue, std::function<void()> foldChanged)
        : value (slotValue), onFoldChanged (std::move (foldChanged))
    {
        if (value == damp::BoardOrder::amp)
        {
            setTooltip ("The amp and its cabinet (set on the Amp tab). Pedals before it feed the amp; those after it take its output.");
            return;
        }
        const auto& info = damp::pedalInfo ((damp::PedalId) value);
        const auto accent = accentFor (info.id);
        for (auto pid : info.params)
            params.push_back (&damp::paramInfo (pid));
        section = std::make_unique<ParamSection> (juce::String (info.name.data(), info.name.size()), p.apvts, params,
                                                  accent, ParamSection::Size::Compact);
        section->setInterceptsMouseClicks (false, true); // its empty areas (the title) drag the tile
        addAndMakeVisible (*section);

        const auto& on = damp::paramInfo (info.onParam);
        onSwitch.setColour (juce::ToggleButton::tickColourId, accent);
        onSwitch.setTooltip (dustify::toJuce (on.name));
        addAndMakeVisible (onSwitch);
        onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, dustify::toJuce (on.id), onSwitch);

        fold.setComponentID ("fold_" + juce::String (info.key.data(), info.key.size()));
        fold.setTooltip ("Show or hide the controls");
        fold.onClick = [this] { toggleFold(); };
        addAndMakeVisible (fold);
        setTooltip ("Click the name to show or hide the controls; drag to move the pedal");

        wasOn = onSwitch.getToggleState();
        for (const auto* spec : params)
            section->setActive (*spec, wasOn);
        section->setCollapsed (true);
        update();
    }

    bool isAmp() const { return section == nullptr; }

    int preferredWidth() const
    {
        if (isAmp())
            return ampWidth;
        const int titleStrip = section->titleWidth() + 8 + foldWidth + switchWidth + 6;
        return showsControls ? std::max (section->preferredWidth(), titleStrip) : titleStrip;
    }

    int preferredHeight() const
    {
        if (isAmp())
            return ampHeight;
        return showsControls ? section->preferredHeight() : section->titleHeight();
    }

    // Polled: follows the pedal's switch. True when the tile changed height.
    bool update()
    {
        if (isAmp())
            return false;
        const bool on = onSwitch.getToggleState();
        if (on != wasOn)
        {
            wasOn = on;
            folding = Folding::Auto; // a pedal switched on opens, one switched off folds
            for (const auto* spec : params)
                section->setActive (*spec, on); // an open pedal that's off: its controls do nothing yet
            repaint(); // the outline follows the switch
        }
        const bool open = folding == Folding::Open || (folding == Folding::Auto && on);
        if (open == showsControls)
            return false;
        showsControls = open;
        section->setCollapsed (! open);
        fold.open = open;
        fold.repaint();
        return true;
    }

    void paint (juce::Graphics& g) override
    {
        if (! isAmp())
            return;
        auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (P::panel).brighter (0.06f));
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (juce::Colour (P::envelope));
        g.drawRoundedRectangle (area, 6.0f, 1.5f);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        const float split = std::min (area.getHeight() * 0.62f, 40.0f);
        g.drawText ("AMP", area.withHeight (split).withTrimmedTop (4.0f), juce::Justification::centred);
        g.setColour (juce::Colour (P::dimText));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("+ cab", area.withTrimmedTop (split - 2.0f), juce::Justification::centredTop);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // A lit pedal's outline in its colour
        if (! isAmp() && onSwitch.getToggleState())
        {
            g.setColour (accentFor ((damp::PedalId) value).withAlpha (0.55f));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f, 1.0f);
        }
    }

    void resized() override
    {
        if (isAmp())
            return;
        section->setBounds (getLocalBounds());
        auto strip = getLocalBounds().removeFromTop (section->titleHeight()).withTrimmedRight (6);
        onSwitch.setBounds (strip.removeFromRight (switchWidth));
        fold.setBounds (strip.removeFromRight (foldWidth));
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 4)
            return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            if (! container->isDragAndDropActive())
                container->startDragging (juce::var (value), this, juce::ScaledImage (createComponentSnapshot (getLocalBounds())));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isAmp() && e.mouseWasClicked() && e.y < section->titleHeight())
            toggleFold();
    }

    void mouseEnter (const juce::MouseEvent&) override { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }

    const int value;

private:
    // Open shows the controls whatever the switch, Closed hides them, Auto shows them while it's on
    enum class Folding { Auto, Open, Closed };

    void toggleFold()
    {
        folding = showsControls ? Folding::Closed : Folding::Open;
        if (update() && onFoldChanged)
            onFoldChanged();
    }

    std::function<void()> onFoldChanged;
    std::unique_ptr<ParamSection> section;
    std::vector<const dustify::ParamSpec*> params;
    juce::ToggleButton onSwitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    FoldButton fold;
    Folding folding = Folding::Auto;
    bool wasOn = false, showsControls = false;
};

PedalboardView::PedalboardView (DampAudioProcessor& p) : processor (p)
{
    refresh (layoutWidth);
}

PedalboardView::~PedalboardView() = default;

void PedalboardView::refresh (int width)
{
    bool changed = width != layoutWidth || tiles.empty();
    layoutWidth = width;
    for (auto& t : tiles)
        changed = t->update() || changed; // a pedal switched on or off opens or folds

    const auto current = processor.getBoard();
    if (current != shown || tiles.empty())
    {
        shown = current;
        changed = true;
        // Keep the existing tiles (their controls, and whether they're open) and just reorder them
        std::vector<std::unique_ptr<Tile>> reordered;
        for (int v : shown.slots)
        {
            auto it = std::find_if (tiles.begin(), tiles.end(), [v] (const auto& t) { return t != nullptr && t->value == v; });
            if (it != tiles.end())
                reordered.push_back (std::move (*it));
            else
                reordered.push_back (std::make_unique<Tile> (processor, v, [this] { layout(); }));
            addAndMakeVisible (*reordered.back());
        }
        tiles = std::move (reordered);
    }

    if (changed)
        layout();
}

void PedalboardView::layout()
{
    // Flow left to right, wrapping. The amp starts a row, so the pedals after it begin there, and
    // fills the row's height. Tiles hang from the top of their row, as tall as its tallest tile.
    rows.clear();
    rowOf.clear();
    int x = margin;
    for (auto& t : tiles)
    {
        const int w = t->preferredWidth();
        if (rows.empty() || (x > margin && (x + w > layoutWidth - margin || t->isAmp())))
        {
            const int y = rows.empty() ? margin : rows.back().getBottom() + rowGap;
            rows.push_back ({ margin, y, layoutWidth - 2 * margin, 0 });
            x = margin;
        }
        auto& row = rows.back();
        row.setHeight (std::max (row.getHeight(), t->preferredHeight()));
        t->setBounds (x, row.getY(), w, t->preferredHeight());
        rowOf.push_back ((int) rows.size() - 1);
        x += w + gap;
    }
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i]->isAmp())
            tiles[i]->setBounds (tiles[i]->getBounds().withHeight (rows[(size_t) rowOf[i]].getHeight()));

    setSize (layoutWidth, rows.back().getBottom() + margin);
    repaint();
}

void PedalboardView::resized() {}

void PedalboardView::paint (juce::Graphics& g)
{
    // Signal flow: a chevron in each gap, level with the title strips
    g.setColour (juce::Colour (P::dimText).withAlpha (0.6f));
    for (size_t i = 0; i + 1 < tiles.size(); ++i)
    {
        if (rowOf[i] != rowOf[i + 1])
            continue;
        const auto a = tiles[i]->getBounds(), b = tiles[i + 1]->getBounds();
        const float cx = (float) (a.getRight() + b.getX()) * 0.5f, cy = (float) a.getY() + 14.0f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 3.0f, cy - 6.0f);
        chevron.lineTo (cx + 3.0f, cy);
        chevron.lineTo (cx - 3.0f, cy + 6.0f);
        g.strokePath (chevron, juce::PathStrokeType (2.0f));
    }
}

void PedalboardView::paintOverChildren (juce::Graphics& g)
{
    if (! drop.has_value())
        return;
    g.setColour (dropAllowed ? juce::Colour (P::envelope) : juce::Colour (0xffe5484d));
    g.fillRoundedRectangle (drop->marker.toFloat(), 2.0f);
}

PedalboardView::Insertion PedalboardView::insertionAt (juce::Point<int> p) const
{
    // The row nearest the pointer
    size_t row = 0;
    for (size_t r = 1; r < rows.size(); ++r)
        if (p.y >= rows[r].getY() - rowGap / 2)
            row = r;

    // Then the nearest gap on that row: before one of its tiles, or after its last
    Insertion best { (int) tiles.size(), {} };
    int bestDistance = std::numeric_limits<int>::max();
    auto consider = [&] (int index, int x)
    {
        if (const int d = std::abs (p.x - x); d < bestDistance)
        {
            bestDistance = d;
            best = { index, { x - 2, rows[row].getY(), 4, rows[row].getHeight() } };
        }
    };
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        if (rowOf[i] != (int) row)
            continue;
        consider ((int) i, tiles[i]->getX() - gap / 2);
        if (i + 1 == tiles.size() || rowOf[i + 1] != (int) row)
            consider ((int) i + 1, std::min (tiles[i]->getRight() + gap / 2, getWidth() - 3));
    }
    return best;
}

damp::BoardOrder PedalboardView::withMove (int slotValue, int index) const
{
    std::vector<int> order (shown.slots.begin(), shown.slots.end());
    const auto from = std::find (order.begin(), order.end(), slotValue) - order.begin();
    order.erase (order.begin() + from);
    if (index > from)
        --index;
    order.insert (order.begin() + std::clamp (index, 0, (int) order.size()), slotValue);
    damp::BoardOrder b;
    std::copy (order.begin(), order.end(), b.slots.begin());
    return b;
}

bool PedalboardView::allowed (int slotValue, int index) const
{
    const auto b = withMove (slotValue, index);
    for (int i = 0; i < damp::BoardOrder::size; ++i)
    {
        const int v = b.slots[(size_t) i];
        if (v == damp::BoardOrder::amp)
            continue;
        const auto& info = damp::pedalInfo ((damp::PedalId) v);
        if (i < b.ampPosition() ? ! info.beforeAmpAllowed() : ! info.afterAmpAllowed())
            return false;
    }
    return true;
}

bool PedalboardView::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.isInt();
}

void PedalboardView::itemDragMove (const SourceDetails& d)
{
    drop = insertionAt (d.localPosition);
    dropAllowed = allowed ((int) d.description, drop->index);
    repaint();
}

void PedalboardView::itemDragExit (const SourceDetails&)
{
    drop.reset();
    repaint();
}

void PedalboardView::itemDropped (const SourceDetails& d)
{
    const int index = insertionAt (d.localPosition).index;
    if (allowed ((int) d.description, index))
        processor.setBoard (withMove ((int) d.description, index));
    drop.reset();
    refresh (layoutWidth);
    repaint();
}
