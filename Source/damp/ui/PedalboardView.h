#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/pedals/Pedal.h"

#include "common/ui/ParamSection.h"

#include <optional>

class DampAudioProcessor;

// The pedalboard: one tile per pedal (and one for the amp) in signal order, wrapping onto rows.
// A pedal that's off folds down to its title strip (name and switch); clicking the name unfolds
// it, for the editor's lifetime or until the pedal is switched. Drag a tile by its title to move
// it; a clipping pedal can't be dropped after the amp.
class PedalboardView final : public juce::Component,
                             public juce::DragAndDropTarget
{
public:
    explicit PedalboardView (DampAudioProcessor&);
    ~PedalboardView() override;

    // Re-reads the board from the processor (after a preset load, say) and lays out for `width`
    void refresh (int width);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    class Tile;

    // Where a dragged tile would land: before tiles[index], marked by `marker`
    struct Insertion
    {
        int index;
        juce::Rectangle<int> marker;
    };

    void layout();
    Insertion insertionAt (juce::Point<int>) const;
    bool allowed (int slotValue, int index) const;
    damp::BoardOrder withMove (int slotValue, int index) const;

    DampAudioProcessor& processor;
    damp::BoardOrder shown;
    std::vector<std::unique_ptr<Tile>> tiles; // in slot order
    std::vector<juce::Rectangle<int>> rows;
    std::vector<int> rowOf; // each tile's row
    int layoutWidth = 1000;
    std::optional<Insertion> drop; // while a tile is dragged over the board
    bool dropAllowed = false;
};
