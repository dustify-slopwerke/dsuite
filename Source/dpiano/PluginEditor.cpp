#include "PluginEditor.h"

DpianoAudioProcessorEditor::DpianoAudioProcessorEditor (DpianoAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      presets (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    using enum dpiano::PID;
    using P = DustifyLookAndFeel::Palette;
    setLookAndFeel (&lookAndFeel);

    title.setText ("dpiano", juce::dontSendNotification);
    title.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, juce::Colour (P::envelope));
    content.addAndMakeVisible (title);
    content.addAndMakeVisible (presets);

    using Page = SectionPage<dpiano::PID>;
    constexpr int fillWidth = designWidth - 16;
    mainPage = std::make_unique<Page> (processor.apvts, std::initializer_list<std::initializer_list<Page::Spec>> {
        {
            { "Hammers", 300, { HammerHardness, StrikePosition, VelocityCurve }, P::source },
            { "Strings", 520, { Inharmonicity, UnisonDetune, Aftersound, Decay, Brilliance, DamperStrength }, P::filter },
            { "Body", 404, { Sympathetic, SoundboardBody, Lid, MechanicalNoise }, P::envelope },
        },
        {
            { "Tuning", 430, { StretchTuning, MasterTune, Temperament, TemperamentRoot }, P::modulation },
            { "Microphones / Room", 590, { Perspective, StereoWidth, MicDistance, RoomMix, RoomSize }, P::effects },
            { "Output", 204, { MasterVolume }, P::global },
        } }, fillWidth);

    // The model's physical constants, set from recordings of a real grand; adjust by ear
    voicingPage = std::make_unique<Page> (processor.apvts, std::initializer_list<std::initializer_list<Page::Spec>> {
        {
            { "Hammer Voicing", 560, { TrebleHardness, FeltNonlinearity, DynamicRange, Knock }, P::source },
            { "Soundboard Response", 664, { Bloom, BoardDrain, BassRadiation, Warmth, Air }, P::envelope },
        } }, fillWidth);

    const auto tabColour = juce::Colour (P::background);
    tabs.addTab ("Piano", tabColour, mainPage.get(), false);
    voicingTab.addAndMakeVisible (*voicingPage);
    voicingTab.addAndMakeVisible (keyboardPanel);
    voicingTab.onResize = [this]
    {
        auto area = voicingTab.getLocalBounds();
        voicingPage->setBounds (area.removeFromTop (voicingPage->preferredHeight()));
        keyboardPanel.setBounds (area.removeFromTop (Page::rowHeight).removeFromLeft (560));
    };
    tabs.addTab ("Voicing", tabColour, &voicingTab, false);
    tabs.setTabBarDepth (30);
    tabs.setOutline (0);
    tabs.setCurrentTabIndex (processor.editorTab, false);
    // addTab() queues a change message; deliver it now so nothing is pending if a host destroys
    // the editor from another thread before the message loop runs
    tabs.getTabbedButtonBar().dispatchPendingMessages();
    content.addAndMakeVisible (tabs);

    // The full 88 keys; computer keys Z S X D C V ... start at middle C
    keyboard.setKeyPressBaseOctave (5);
    keyboard.setOctaveForMiddleC (4);
    keyboard.setAvailableRange (dpiano::lowestKey, dpiano::highestKey);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, juce::Colour (P::envelope).withAlpha (0.7f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, juce::Colour (P::envelope).withAlpha (0.25f));
    keyboard.setColour (juce::MidiKeyboardComponent::shadowColourId, juce::Colours::black.withAlpha (0.4f));
    keyboard.setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour (0xff9aa0a8));
    content.addAndMakeVisible (keyboard);

    content.onResize = [this] { layoutContent(); };
    content.setSize (designWidth, designHeight);
    addAndMakeVisible (content);

    const float startScale = processor.uiScale; // read first: the calls below resize, and resized() saves the scale
    setResizable (true, true);
    setResizeLimits (designWidth * 6 / 10, designHeight * 6 / 10, designWidth * 2, designHeight * 2);
    if (auto* sizeLimits = getConstrainer())
        sizeLimits->setFixedAspectRatio ((double) designWidth / designHeight);
    setSize (juce::roundToInt (designWidth * startScale), juce::roundToInt (designHeight * startScale));

    // Grab focus once the window is on screen so the computer keyboard plays straight away
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer (&keyboard)]
    {
        if (safe != nullptr && safe->isShowing())
            safe->grabKeyboardFocus();
    });
}

DpianoAudioProcessorEditor::~DpianoAudioProcessorEditor()
{
    processor.editorTab = tabs.getCurrentTabIndex();
    setLookAndFeel (nullptr);
    tabs.getTabbedButtonBar().dispatchPendingMessages();
    // A ChangeBroadcaster with no listeners of ours: flush any queued change message so none is
    // pending if the host deletes us off the message thread
    keyboard.dispatchPendingMessages();
}

void DpianoAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (DustifyLookAndFeel::Palette::background));
}

void DpianoAudioProcessorEditor::resized()
{
    const float scale = (float) getWidth() / (float) designWidth;
    processor.uiScale = scale;
    content.setTransform (juce::AffineTransform::scale (scale));
}

void DpianoAudioProcessorEditor::layoutContent()
{
    auto area = content.getLocalBounds().reduced (8);

    auto top = area.removeFromTop (36);
    title.setBounds (top.removeFromLeft (140));
    presets.setBounds (top.removeFromRight (420));
    area.removeFromTop (4);

    keyboard.setBounds (area.removeFromBottom (90));
    keyboard.setKeyWidth ((float) keyboard.getWidth() / 52.0f); // 52 white keys
    area.removeFromBottom (6);

    tabs.setBounds (area);
}
