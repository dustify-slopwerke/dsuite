#include "PresetBar.h"

namespace
{
constexpr int userPresetIdBase = 1000;
} // namespace

PresetBar::PresetBar (dustify::PresetHost& h) : host (h)
{
    box.setTextWhenNothingSelected ("Presets");
    box.onChange = [this] { chosen(); };
    addAndMakeVisible (box);
    saveButton.onClick = [this] { save(); };
    addAndMakeVisible (saveButton);

    auto step = [this] (int delta)
    {
        const int n = box.getNumItems();
        if (n > 0)
            box.setSelectedItemIndex ((box.getSelectedItemIndex() + delta + n) % n);
    };
    prevButton.onClick = [step] { step (-1); };
    nextButton.onClick = [step] { step (1); };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);

    refreshList();
    timerCallback();
    startTimerHz (5);
}

void PresetBar::resized()
{
    auto area = getLocalBounds();
    saveButton.setBounds (area.removeFromRight (80).reduced (2, 4));
    nextButton.setBounds (area.removeFromRight (32).reduced (2, 4));
    prevButton.setBounds (area.removeFromLeft (32).reduced (2, 4));
    box.setBounds (area.reduced (2, 4));
}

void PresetBar::timerCallback()
{
    const auto name = host.getCurrentPresetName();
    if (name.isNotEmpty() && box.getText() != name)
        box.setText (name, juce::dontSendNotification);
}

void PresetBar::refreshList()
{
    box.clear (juce::dontSendNotification);

    // Factory presets are stored in category order; each new category gets a heading
    const auto& factory = host.getFactoryPresets();
    for (size_t i = 0; i < factory.size(); ++i)
    {
        const auto& category = factory[i].category;
        if (category.isNotEmpty() && (i == 0 || category != factory[i - 1].category))
        {
            if (i > 0)
                box.addSeparator();
            box.addSectionHeading (category);
        }
        box.addItem (factory[i].name, (int) i + 1);
    }

    userPresets = host.userPresetDirectory().findChildFiles (juce::File::findFiles, false, "*." + host.presetFileExtension());
    userPresets.sort();
    if (! userPresets.isEmpty())
    {
        box.addSeparator();
        box.addSectionHeading ("User");
        for (int i = 0; i < userPresets.size(); ++i)
            box.addItem (userPresets[i].getFileNameWithoutExtension(), userPresetIdBase + i);
    }
}

void PresetBar::chosen()
{
    const int id = box.getSelectedId();
    if (id >= userPresetIdBase)
    {
        const auto& file = userPresets[id - userPresetIdBase];
        if (file.existsAsFile())
            host.loadPresetText (file.loadFileAsString().toStdString());
    }
    else if (id > 0)
    {
        host.loadFactoryPreset (id - 1);
    }
}

void PresetBar::save()
{
    const auto dir = host.userPresetDirectory();
    dir.createDirectory();
    const auto extension = host.presetFileExtension();

    fileChooser = std::make_unique<juce::FileChooser> ("Save preset", dir.getChildFile ("My Preset." + extension), "*." + extension);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, extension] (const juce::FileChooser& chooser)
                              {
                                  auto file = chooser.getResult();
                                  if (file == juce::File())
                                      return;
                                  file = file.withFileExtension (extension);
                                  file.replaceWithText (host.savePresetText (file.getFileNameWithoutExtension()));
                                  host.setCurrentPresetName (file.getFileNameWithoutExtension());
                                  refreshList();
                                  box.setText (file.getFileNameWithoutExtension(), juce::dontSendNotification);
                              });
}
