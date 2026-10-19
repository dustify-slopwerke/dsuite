#include "PluginEditor.h"

#include "common/plugin/ParamLayout.h"

DampAudioProcessorEditor::DampAudioProcessorEditor (DampAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      presets (p),
      pedalboard (p),
      looperView (p),
      tuner (p)
{
    using enum damp::PID;
    using P = DustifyLookAndFeel::Palette;
    setLookAndFeel (&lookAndFeel);

    title.setText ("damp", juce::dontSendNotification);
    title.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, juce::Colour (P::envelope));
    content.addAndMakeVisible (title);
    content.addAndMakeVisible (presets);

    using Page = SectionPage<damp::PID>;
    page = std::make_unique<Page> (processor.apvts, std::initializer_list<std::initializer_list<Page::Spec>> {
        {
            { "Amp", 620, { Model, Gain, Bright, Bass, Middle, Treble }, P::filter },
            { "Power Amp", 412, { Master, Presence, Resonance, Sag }, P::envelope },
        },
        {
            { "Input", 110, { InputGain }, P::source },
            { "Noise Gate", 310, { GateOn, GateThreshold, GateRelease }, P::modulation },
            { "Output", 234, { Oversampling, OutputVolume }, P::global },
            { "Cabinet", 378, { Cab, Mic, MicPosition, MicDistance }, P::effects },
        } }, designWidth - 16);
    ampTab.addAndMakeVisible (*page);
    pedalViewport.setViewedComponent (&pedalboard, false);
    pedalViewport.setScrollBarsShown (true, false);

    const auto tabColour = juce::Colour (P::background);
    tabs.addTab ("Amp", tabColour, &ampTab, false);
    tabs.addTab ("Pedals", tabColour, &pedalViewport, false);
    tabs.addTab ("Looper", tabColour, &looperView, false);
    tabs.setTabBarDepth (28);
    tabs.setOutline (0);
    tabs.getTabbedButtonBar().dispatchPendingMessages();
    content.addAndMakeVisible (tabs);

    loadImpulse.setTooltip ("Load a cabinet impulse response (WAV, AIFF, FLAC). It is saved with the session.");
    loadImpulse.onClick = [this] { chooseImpulse(); };
    ampTab.addAndMakeVisible (loadImpulse);
    impulseName.setFont (juce::FontOptions (13.0f));
    impulseName.setColour (juce::Label::textColourId, juce::Colour (P::dimText));
    ampTab.addAndMakeVisible (impulseName);
    tunerButton.setComponentID ("tunerButton");
    tunerButton.setClickingTogglesState (true);
    tunerButton.setTooltip ("Chromatic tuner (mutes the output while open, unless you turn that off)");
    tunerButton.onClick = [this] { tuner.setVisible (tunerButton.getToggleState()); };
    content.addAndMakeVisible (tunerButton);
    inputMeter.setSweetSpot (-18.0f, -6.0f);
    inputMeter.setTooltip ("Input level. Hard picking should peak in the shaded range: set your interface (or Input) so it does.");
    outputMeter.setTooltip ("Output level");
    content.addAndMakeVisible (inputMeter);
    content.addAndMakeVisible (outputMeter);
    content.addChildComponent (tuner);

    lastTick = juce::Time::getMillisecondCounter();
    timerCallback();
    startTimerHz (30);

    content.onResize = [this] { layoutContent(); };
    content.setSize (designWidth, designHeight);
    addAndMakeVisible (content);

    const float startScale = processor.uiScale; // read first: the calls below resize, and resized() saves the scale
    setResizable (true, true);
    setResizeLimits (designWidth * 6 / 10, designHeight * 6 / 10, designWidth * 2, designHeight * 2);
    if (auto* sizeLimits = getConstrainer())
        sizeLimits->setFixedAspectRatio ((double) designWidth / designHeight);
    setSize (juce::roundToInt (designWidth * startScale), juce::roundToInt (designHeight * startScale));
}

DampAudioProcessorEditor::~DampAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
    tabs.getTabbedButtonBar().dispatchPendingMessages();
}

void DampAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (DustifyLookAndFeel::Palette::background));
}

void DampAudioProcessorEditor::resized()
{
    const float scale = (float) getWidth() / (float) designWidth;
    processor.uiScale = scale;
    content.setTransform (juce::AffineTransform::scale (scale));
}

void DampAudioProcessorEditor::layoutContent()
{
    auto area = content.getLocalBounds().reduced (8);

    auto top = area.removeFromTop (36);
    title.setBounds (top.removeFromLeft (90));
    presets.setBounds (top.removeFromRight (420));
    tunerButton.setBounds (top.removeFromLeft (72).reduced (0, 4));
    top.removeFromLeft (16);
    auto meters = top.reduced (0, 2);
    inputMeter.setBounds (meters.removeFromTop (meters.getHeight() / 2).withTrimmedRight (16));
    outputMeter.setBounds (meters.withTrimmedRight (16));
    area.removeFromTop (4);

    tabs.setBounds (area);
    tuner.setBounds (area.withSizeKeepingCentre (620, 300));
    const auto inside = tabs.getTabbedButtonBar().getBounds().getHeight();
    page->setBounds (0, 0, area.getWidth(), area.getHeight() - inside);
    pedalboard.refresh (area.getWidth() - pedalViewport.getScrollBarThickness());

    // Under the cabinet section's controls (the last section of the second row)
    const int cabX = 110 + 310 + 234, rowY = SectionPage<damp::PID>::rowHeight;
    auto strip = juce::Rectangle<int> (cabX + 10, rowY + 144, 358, 26);
    loadImpulse.setBounds (strip.removeFromLeft (96));
    strip.removeFromLeft (8);
    impulseName.setBounds (strip);
}

void DampAudioProcessorEditor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const float seconds = (float) (now - lastTick) / 1000.0f;
    lastTick = now;

    inputMeter.setPeak (processor.takeInputPeak(), seconds);
    outputMeter.setPeak (processor.takeOutputPeak(), seconds);
    tuner.update (seconds);
    looperView.update();
    updateRelevance();
    pedalboard.refresh (pedalViewport.getWidth() - pedalViewport.getScrollBarThickness()); // a preset may have moved pedals

    const auto name = processor.getUserImpulseName();
    impulseName.setText (name.isEmpty() ? "No IR loaded" : name, juce::dontSendNotification);
}

// Dims controls that do nothing in the current setup
void DampAudioProcessorEditor::updateRelevance()
{
    using enum damp::PID;
    auto value = [this] (damp::PID pid) { return processor.apvts.getRawParameterValue (dustify::toJuce (damp::paramInfo (pid).id))->load(); };

    const bool gateOn = value (GateOn) >= 0.5f;
    page->setActive (GateThreshold, gateOn);
    page->setActive (GateRelease, gateOn);

    const auto model = (damp::AmpModel) juce::roundToInt (value (Model));
    page->setActive (Resonance, damp::modelSpec (model).power.feedback > 0.0); // no feedback loop for it to act on

    const auto cab = (damp::CabType) juce::roundToInt (value (Cab));
    const bool modelled = cab != damp::CabType::Off && cab != damp::CabType::User;
    for (auto pid : { Mic, MicPosition, MicDistance })
        page->setActive (pid, modelled);
}

void DampAudioProcessorEditor::chooseImpulse()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a cabinet impulse response", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              juce::String error;
                              if (! safe->processor.loadUserImpulse (fc.getResult(), error))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Load IR", error);
                              safe->timerCallback();
                          });
}
