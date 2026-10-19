#include "PluginEditor.h"

#include "ui/Relevance.h"

namespace
{
constexpr int userPresetIdBase = 1000;
constexpr int keyboardLowest = 24, keyboardHighest = 108; // C1..C8, 50 white keys

} // namespace

DsynthAudioProcessorEditor::DsynthAudioProcessorEditor (DsynthAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    using enum dsynth::PID;
    setLookAndFeel (&lookAndFeel);

    title.setText ("dsynth", juce::dontSendNotification);
    title.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, juce::Colour (DustifyLookAndFeel::Palette::source));
    content.addAndMakeVisible (title);

    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this] { presetChosen(); };
    content.addAndMakeVisible (presetBox);
    saveButton.onClick = [this] { savePreset(); };
    content.addAndMakeVisible (saveButton);

    auto step = [this] (int delta)
    {
        const int n = presetBox.getNumItems();
        if (n > 0)
            presetBox.setSelectedItemIndex ((presetBox.getSelectedItemIndex() + delta + n) % n);
    };
    prevButton.onClick = [step] { step (-1); };
    nextButton.onClick = [step] { step (1); };
    content.addAndMakeVisible (prevButton);
    content.addAndMakeVisible (nextButton);

    for (auto* b : { &editA, &editB })
    {
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (DustifyLookAndFeel::Palette::source));
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        content.addAndMakeVisible (*b);
    }
    editA.onClick = [this] { processor.setEditLayer (0); timerCallback(); };
    editB.onClick = [this] { processor.setEditLayer (1); timerCallback(); };
    copyLayer.setTooltip ("Copy the layer being edited to the other layer");
    copyLayer.onClick = [this] { processor.copyEditLayerToOther(); };
    content.addAndMakeVisible (copyLayer);
    refreshPresetList();

    using P = DustifyLookAndFeel::Palette;
    using Page = SectionPage<dsynth::PID>;
    constexpr int fillWidth = 1200;
    voicePage = std::make_unique<Page> (processor.apvts, std::initializer_list<std::initializer_list<Page::Spec>> {
        {
            { "Oscillator 1", 360, { Osc1On, Osc1Wave, Osc1Range, Osc1Sync, Osc1Tune, Osc1PW, Osc1Level }, P::source },
            { "Oscillator 2", 360, { Osc2On, Osc2Wave, Osc2Range, Osc2KeyTrack, Osc2Tune, Osc2PW, Osc2Level }, P::source },
            { "Oscillator 3", 360, { Osc3On, Osc3Wave, Osc3Range, Osc3KeyTrack, Osc3Tune, Osc3PW, Osc3Level }, P::source },
            { "Mixer / Source", 390, { NoiseLevel, NoiseColour, SubLevel, SubOctave, MixerDrive, RingMod, CrossMod, OscType, PhaseReset }, P::source },
        },
        {
            { "Filter", 385, { FilterModel, FilterCutoff, FilterReso, FilterEnvAmt, FilterKeyTrack, FilterResComp, FilterVel, FilterShape, FilterBandpass }, P::filter },
            { "Pre-HPF", 145, { VoiceHpfCutoff, VoiceHpfReso }, P::filter },
            { "Filter Envelope", 278, { FEnvA, FEnvD, FEnvS, FEnvR, EnvCurve }, P::envelope },
            { "Amp Envelope", 302, { AEnvA, AEnvD, AEnvS, AEnvR, EnvReleaseMode, AmpVel, VcaMode }, P::envelope },
            { "Voicing", 434, { VoiceMode, NotePriority, KeyAssign, GlideTime, BendRange, GlideType, GlideMode, UnisonVoices, UnisonDetune }, P::envelope },
        } }, fillWidth);

    modPage = std::make_unique<Page> (processor.apvts, std::initializer_list<std::initializer_list<Page::Spec>> {
        {
            { "Poly-Mod", 210, { PmFilterEnv, PmOsc2, PmToFreq, PmToPW, PmToFilter }, P::modulation },
            { "LFO", 250, { LfoRate, LfoWave, LfoDelay, LfoKeySync }, P::modulation },
            { "Wheel Mod", 260, { ModSource, ModMix, ModAmount, ModToPitch, ModToFilter, ModToPW }, P::modulation },
            { "Mod Matrix", 600, { Mx1Source, Mx1Dest, Mx1Amount, Mx2Source, Mx2Dest, Mx2Amount,
                                   Mx3Source, Mx3Dest, Mx3Amount, Mx4Source, Mx4Dest, Mx4Amount }, P::modulation },
        },
        {
            { "Arpeggiator", 370, { ArpOn, ArpMode, ArpRate, ArpOctaves, ArpGate, ArpLatch, ArpSync, ArpTempo }, P::modulation },
            { "LFO 2 (per voice, via matrix)", 300, { Lfo2Rate, Lfo2Wave, Lfo2Delay, Lfo2KeySync }, P::modulation },
        } }, fillWidth);

    effectsPage = std::make_unique<Page> (processor.apvts, std::initializer_list<std::initializer_list<Page::Spec>> {
        {
            { "Chorus / HPF", 250, { ChorusMode, HpfMode }, P::effects },
            { "Delay", 370, { DelayMode, DelaySync, DelayNote, DelayTime, DelayFeedback, DelayTone, DelayMix }, P::effects },
            { "Reverb", 210, { ReverbMix, ReverbSize, ReverbDecay, ReverbDamping, ReverbPredelay }, P::effects },
        },
        {
            { "Analog / Out", 210, { Drift, Tolerance, PanSpread, MasterTune, MasterVolume }, P::global },
            { "Layers (shared)", 330, { LayerMode, SplitNote, LayerBalance, LayerDetune }, P::global },
            { "MPE (shared)", 150, { MpeMode, MpeBendRange }, P::global },
            { "Keys / Tuning (shared)", 350, { VelocityCurve, Tuning, TuningRoot }, P::global },
            { "Engine (shared)", 150, { Quality }, P::global },
        } }, fillWidth);

    const auto tabColour = juce::Colour (P::background);
    tabs.addTab ("Voice", tabColour, voicePage.get(), false);
    tabs.addTab ("Modulation", tabColour, modPage.get(), false);
    tabs.addTab ("Effects / Global", tabColour, effectsPage.get(), false);
    tabs.setTabBarDepth (30);
    tabs.setOutline (0);
    tabs.setCurrentTabIndex (processor.editorTab, false);
    // addTab() queues a change message; deliver it now so nothing is pending if a host destroys
    // the editor from another thread before the message loop runs
    tabs.getTabbedButtonBar().dispatchPendingMessages();
    content.addAndMakeVisible (tabs);

    // Computer keys: Z S X D C V G B H N J M ... starting at middle C
    keyboard.setKeyPressBaseOctave (5);
    keyboard.setOctaveForMiddleC (4);
    keyboard.setAvailableRange (keyboardLowest, keyboardHighest);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, juce::Colour (P::source).withAlpha (0.7f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, juce::Colour (P::source).withAlpha (0.25f));
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

    timerCallback(); // initial layer buttons, preset name and control dimming
    startTimerHz (10);

    // Grab focus once the window is on screen so the computer keyboard plays straight away
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer (&keyboard)]
    {
        if (safe != nullptr && safe->isShowing())
            safe->grabKeyboardFocus();
    });
}

DsynthAudioProcessorEditor::~DsynthAudioProcessorEditor()
{
    processor.editorTab = tabs.getCurrentTabIndex();
    setLookAndFeel (nullptr);

    // Both are ChangeBroadcasters (an AsyncUpdater inside) with no listeners of ours: flush any
    // queued change message so none is pending if the host deletes us off the message thread
    keyboard.dispatchPendingMessages();
    tabs.getTabbedButtonBar().dispatchPendingMessages();
}

void DsynthAudioProcessorEditor::timerCallback()
{
    const int layer = processor.getEditLayer();
    editA.setToggleState (layer == 0, juce::dontSendNotification);
    editB.setToggleState (layer == 1, juce::dontSendNotification);
    copyLayer.setButtonText (layer == 0 ? "Copy A > B" : "Copy B > A");

    // Dim controls that currently have no effect
    const auto patch = processor.livePatch();
    for (const auto& info : dsynth::allParams())
    {
        const bool relevant = dsynth::isRelevant (info.pid, patch);
        for (auto* page : { voicePage.get(), modPage.get(), effectsPage.get() })
            if (page != nullptr) // the first call comes before the pages are built
                page->setActive (info.pid, relevant);
    }

    // Follow presets loaded from elsewhere (host program changes, session restore)
    const auto presetName = processor.getCurrentPresetName();
    if (presetName.isNotEmpty() && presetBox.getText() != presetName)
        presetBox.setText (presetName, juce::dontSendNotification);
}

void DsynthAudioProcessorEditor::refreshPresetList()
{
    presetBox.clear (juce::dontSendNotification);

    // Factory presets are stored in category order; each new category gets a heading
    const auto& factory = processor.getFactoryPresets();
    for (size_t i = 0; i < factory.size(); ++i)
    {
        const auto& category = factory[i].category;
        if (category.isNotEmpty() && (i == 0 || category != factory[i - 1].category))
        {
            if (i > 0)
                presetBox.addSeparator();
            presetBox.addSectionHeading (category);
        }
        presetBox.addItem (factory[i].name, (int) i + 1);
    }

    userPresets = DsynthAudioProcessor::userPresetDirectory().findChildFiles (juce::File::findFiles, false, "*.dsynth");
    userPresets.sort();
    if (! userPresets.isEmpty())
    {
        presetBox.addSeparator();
        presetBox.addSectionHeading ("User");
        for (int i = 0; i < userPresets.size(); ++i)
            presetBox.addItem (userPresets[i].getFileNameWithoutExtension(), userPresetIdBase + i);
    }
}

void DsynthAudioProcessorEditor::presetChosen()
{
    const int id = presetBox.getSelectedId();
    if (id >= userPresetIdBase)
    {
        const auto& file = userPresets[id - userPresetIdBase];
        if (file.existsAsFile())
            processor.loadPresetText (file.loadFileAsString().toStdString());
    }
    else if (id > 0)
    {
        processor.setCurrentProgram (id - 1);
    }
}

void DsynthAudioProcessorEditor::savePreset()
{
    const auto dir = DsynthAudioProcessor::userPresetDirectory();
    dir.createDirectory();

    fileChooser = std::make_unique<juce::FileChooser> ("Save preset", dir.getChildFile ("My Preset.dsynth"), "*.dsynth");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  auto file = chooser.getResult();
                                  if (file == juce::File())
                                      return;
                                  file = file.withFileExtension ("dsynth");
                                  file.replaceWithText (processor.savePresetText (file.getFileNameWithoutExtension()));
                                  processor.setCurrentPresetName (file.getFileNameWithoutExtension());
                                  refreshPresetList();
                                  presetBox.setText (file.getFileNameWithoutExtension(), juce::dontSendNotification);
                              });
}

void DsynthAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (DustifyLookAndFeel::Palette::background));
}

void DsynthAudioProcessorEditor::resized()
{
    const float scale = (float) getWidth() / (float) designWidth;
    processor.uiScale = scale;
    content.setTransform (juce::AffineTransform::scale (scale));
}

void DsynthAudioProcessorEditor::layoutContent()
{
    auto area = content.getLocalBounds().reduced (8);

    auto top = area.removeFromTop (36);
    title.setBounds (top.removeFromLeft (140));
    editA.setBounds (top.removeFromLeft (70).reduced (2, 4));
    editB.setBounds (top.removeFromLeft (70).reduced (2, 4));
    copyLayer.setBounds (top.removeFromLeft (100).reduced (2, 4));
    saveButton.setBounds (top.removeFromRight (80).reduced (2, 4));
    nextButton.setBounds (top.removeFromRight (32).reduced (2, 4));
    presetBox.setBounds (top.removeFromRight (260).reduced (2, 4));
    prevButton.setBounds (top.removeFromRight (32).reduced (2, 4));
    area.removeFromTop (4);

    keyboard.setBounds (area.removeFromBottom (90));
    keyboard.setKeyWidth ((float) keyboard.getWidth() / 50.0f);
    area.removeFromBottom (6);

    tabs.setBounds (area);
}
