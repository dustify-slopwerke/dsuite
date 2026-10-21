// Paints every tab of a plugin's editor to PNG files, without creating a window, so the UI can be
// checked without anything appearing on screen.
//
//   <plugin>_ui_snapshot <output-dir> [factory-preset-index] [edit-layer: A|B (dsynth)] [ui-scale]
//                        [<param-id>=<value>...] [click=<component-id>...] [looper=ready|demo|countin (damp)]
//
// Parameters are set (in their own units) before the editor opens; buttons named by click= are
// clicked once it has. A tab that scrolls also gets a picture of all its content (tabN-all.png).
// damp: looper=ready switches the looper on with its memory reserved; looper=demo also fills
// three tracks with test audio (track 1 padded before and after and playing, 2 reversed and
// stopped, 3 playing), so the looper tab has something to show; looper=countin presses Rec on
// track 1 with count-in on and stops part way through the count-in bar.
// dpiano: velmap=<min>,<max>,<sensitivity %> sets the keyboard calibration (MIDI velocities);
// notes=<velocity>,... plays note-ons before the editor opens; calnotes=<velocity>,... plays them
// after the clicks (e.g. click=calibrateButton) and lets the keyboard panel see them.
// Settings files are written to a temporary folder (DUSTIFY_SETTINGS_DIR), never the user's.

#include <juce_audio_processors/juce_audio_processors.h>

#if DUSTIFY_SNAPSHOT_DSYNTH || DUSTIFY_SNAPSHOT_DAMP || DUSTIFY_SNAPSHOT_DPIANO
 #include "PluginProcessor.h"
#endif
#if DUSTIFY_SNAPSHOT_DPIANO
 #include "KeyboardPanel.h"
#endif

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <numbers>

namespace
{
juce::TabbedComponent* findTabs (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* tabs = dynamic_cast<juce::TabbedComponent*> (child))
            return tabs;
        if (auto* tabs = findTabs (*child))
            return tabs;
    }
    return nullptr;
}

juce::Component* findById (juce::Component& parent, const juce::String& id)
{
    for (auto* child : parent.getChildren())
    {
        if (child->getComponentID() == id)
            return child;
        if (auto* found = findById (*child, id))
            return found;
    }
    return nullptr;
}

bool writePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    if (! stream.openedOk() || ! juce::PNGImageFormat().writeImageToStream (image, stream))
    {
        std::cerr << "couldn't write " << file.getFullPathName() << "\n";
        return false;
    }
    std::cout << file.getFullPathName() << " (" << image.getWidth() << "x" << image.getHeight() << ")\n";
    return true;
}

#if DUSTIFY_SNAPSHOT_DAMP
void setParam (juce::AudioProcessor& p, const juce::String& id, float value)
{
    for (auto* param : p.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->paramID == id)
            ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
}

void prepareLooper (DampAudioProcessor& p, const juce::String& setup)
{
    constexpr double rate = 48000.0;
    constexpr int block = 512;
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    auto run = [&] (double seconds)
    {
        for (int i = 0; i < (int) std::ceil (seconds * rate / block); ++i)
        {
            buffer.clear();
            p.processBlock (buffer, midi);
        }
        p.getLooper().audioStopped(); // message calls act at once
    };

    p.prepareToPlay (rate, block);
    setParam (p, "looper_on", 1.0f);
    auto& looper = p.getLooper();
    looper.createPool (rate);
    run (0.01);
    if (setup == "countin")
    {
        setParam (p, "count_in", 1.0f);
        run (0.01);
        looper.press (damp::Looper::Action::Rec, 0);
        run (0.6);
        return;
    }
    if (setup != "demo")
        return;

    // Two bars at 120 BPM of plucks, a rising chord (reversed: it falls), and a pulsing bass
    auto make = [] (double seconds, auto&& sample)
    {
        std::vector<float> x ((size_t) (seconds * rate));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (float) sample ((double) i / rate);
        return x;
    };
    const double twoPi = 2.0 * std::numbers::pi;
    const auto plucks = make (2.0, [&] (double t)
                              {
                                  const double since = std::fmod (t, 0.5);
                                  const double f = 220.0 * (1.0 + std::floor (t / 0.5) * 0.25);
                                  return 0.8 * std::exp (-since * 9.0) * std::sin (twoPi * f * since);
                              });
    const auto chord = make (4.0, [&] (double t) { return 0.7 * (t / 4.0) * (std::sin (twoPi * 330.0 * t) + std::sin (twoPi * 495.0 * t)) * 0.5; });
    const auto bass = make (2.0, [&] (double t) { return 0.6 * std::exp (-std::fmod (t, 0.25) * 14.0) * std::sin (twoPi * 110.0 * t); });
    looper.setTrackAudio (0, plucks.data(), nullptr, (int64_t) plucks.size());
    looper.setTrackAudio (1, chord.data(), nullptr, (int64_t) chord.size());
    looper.setTrackAudio (2, bass.data(), nullptr, (int64_t) bass.size());
    looper.setTrackEdits (0, -(int64_t) (0.25 * rate), (int64_t) (2.25 * rate)); // silence either side
    setParam (p, "track2_reverse", 1.0f);
    run (0.05);
    looper.press (damp::Looper::Action::Play, 0);
    looper.press (damp::Looper::Action::Stop, 1);
    looper.press (damp::Looper::Action::Play, 2);
    run (0.9); // the playheads part way through
}
#endif

#if DUSTIFY_SNAPSHOT_DPIANO
void playVelocities (juce::AudioProcessor& p, const juce::String& list)
{
    p.prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    int note = 60;
    for (const auto& v : juce::StringArray::fromTokens (list, ",", ""))
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) juce::jlimit (1, 127, v.getIntValue())), 0);
        midi.addEvent (juce::MidiMessage::noteOff (1, note), 256);
        p.processBlock (buffer, midi);
        note += 2;
    }
}
#endif
} // namespace

int main (int argc, char** argv)
{
    // Positional arguments, then name=value options
    std::vector<juce::String> args, clicks;
    juce::String looperSetup;
    std::map<juce::String, juce::String> pluginOptions; // velmap, notes, calnotes (dpiano)
    std::vector<std::pair<juce::String, float>> settings;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (! arg.contains ("="))
            args.push_back (arg);
        else if (arg.upToFirstOccurrenceOf ("=", false, false) == "click")
            clicks.push_back (arg.fromFirstOccurrenceOf ("=", false, false));
        else if (arg.upToFirstOccurrenceOf ("=", false, false) == "looper")
            looperSetup = arg.fromFirstOccurrenceOf ("=", false, false);
        else if (juce::StringArray { "velmap", "notes", "calnotes" }.contains (arg.upToFirstOccurrenceOf ("=", false, false)))
            pluginOptions[arg.upToFirstOccurrenceOf ("=", false, false)] = arg.fromFirstOccurrenceOf ("=", false, false);
        else
            settings.emplace_back (arg.upToFirstOccurrenceOf ("=", false, false), arg.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
    }
    if (args.empty())
    {
        std::cerr << "usage: " << argv[0] << " <output-dir> [factory-preset-index] [A|B] [ui-scale] [param=value...] [click=component-id...] [looper=ready|demo|countin]\n";
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juce;

    const auto settingsDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("dustify-snapshot", "");
#if JUCE_WINDOWS
    _putenv_s ("DUSTIFY_SETTINGS_DIR", settingsDir.getFullPathName().toRawUTF8());
#else
    setenv ("DUSTIFY_SETTINGS_DIR", settingsDir.getFullPathName().toRawUTF8(), 1);
#endif
    const struct RemoveDir { juce::File dir; ~RemoveDir() { dir.deleteRecursively(); } } removeSettingsDir { settingsDir };

    const juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (args[0]));
    outDir.createDirectory();

    std::unique_ptr<juce::AudioProcessor> processor (createPluginFilter());
    if (args.size() > 1)
        processor->setCurrentProgram (args[1].getIntValue());
    const float uiScale = args.size() > 3 ? args[3].getFloatValue() : 0.0f;
#if DUSTIFY_SNAPSHOT_DSYNTH
    if (args.size() > 2 && args[2].equalsIgnoreCase ("B"))
        dynamic_cast<DsynthAudioProcessor&> (*processor).setEditLayer (1);
    if (uiScale > 0.0f)
        dynamic_cast<DsynthAudioProcessor&> (*processor).uiScale = uiScale;
#endif
    for (const auto& [id, value] : settings)
    {
        auto& params = processor->getParameters();
        auto it = std::find_if (params.begin(), params.end(), [&id] (auto* p)
                                { auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);
                                  return withId != nullptr && withId->paramID == id; });
        auto* ranged = it != params.end() ? dynamic_cast<juce::RangedAudioParameter*> (*it) : nullptr;
        if (ranged == nullptr)
        {
            std::cerr << "no parameter " << id << "\n";
            return 1;
        }
        ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

#if DUSTIFY_SNAPSHOT_DAMP
    if (looperSetup.isNotEmpty())
        prepareLooper (dynamic_cast<DampAudioProcessor&> (*processor), looperSetup);
#endif

#if DUSTIFY_SNAPSHOT_DPIANO
    auto& piano = dynamic_cast<DpianoAudioProcessor&> (*processor);
    if (const auto it = pluginOptions.find ("velmap"); it != pluginOptions.end())
    {
        const auto parts = juce::StringArray::fromTokens (it->second, ",", "");
        dpiano::VelocityMap map;
        map.inMin = parts[0].getFloatValue() / 127.0f;
        map.inMax = parts.size() > 1 ? parts[1].getFloatValue() / 127.0f : 1.0f;
        map.sensitivity = parts[2].getFloatValue() / 100.0f;
        piano.setVelocityMap (map, false);
    }
    if (const auto it = pluginOptions.find ("notes"); it != pluginOptions.end())
        playVelocities (piano, it->second);
#else
    if (! pluginOptions.empty())
    {
        std::cerr << "velmap, notes and calnotes are dpiano options\n";
        return 1;
    }
#endif

    // As a host does: createEditor() is private, and the processor tracks its active editor
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor->createEditorAndMakeActive());
#if ! DUSTIFY_SNAPSHOT_DSYNTH
    if (uiScale > 0.0f && editor->isResizable()) // other editors scale with their size
        editor->setSize (juce::roundToInt ((float) editor->getWidth() * uiScale), juce::roundToInt ((float) editor->getHeight() * uiScale));
#endif
    auto snapshot = [&] { return editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f); };

    for (const auto& id : clicks)
    {
        // Only the current tab's content is in the editor: look through each in turn
        auto* button = dynamic_cast<juce::Button*> (findById (*editor, id));
        if (auto* tabs = findTabs (*editor))
            for (int tab = 0; tab < tabs->getNumTabs() && button == nullptr; ++tab)
            {
                tabs->setCurrentTabIndex (tab, false);
                button = dynamic_cast<juce::Button*> (findById (*editor, id));
            }
        if (button == nullptr || ! button->onClick)
        {
            std::cerr << "no button " << id << "\n";
            return 1;
        }
        button->onClick(); // as a click would, but now rather than via the message queue
    }

#if DUSTIFY_SNAPSHOT_DPIANO
    if (const auto it = pluginOptions.find ("calnotes"); it != pluginOptions.end())
    {
        playVelocities (piano, it->second);
        if (auto* tabs = findTabs (*editor))
            tabs->setCurrentTabIndex (1, false);
        if (auto* panel = dynamic_cast<KeyboardPanel*> (findById (*editor, "keyboardPanel")))
            panel->timerCallback();
    }
#endif

    if (auto* tabs = findTabs (*editor))
    {
        for (int tab = 0; tab < tabs->getNumTabs(); ++tab)
        {
            tabs->setCurrentTabIndex (tab, false);
            if (! writePng (snapshot(), outDir.getChildFile ("tab" + juce::String (tab) + ".png")))
                return 1;
            if (auto* viewport = dynamic_cast<juce::Viewport*> (tabs->getTabContentComponent (tab)))
                if (auto* all = viewport->getViewedComponent(); all != nullptr && all->getHeight() > viewport->getHeight())
                    if (! writePng (all->createComponentSnapshot (all->getLocalBounds(), true, 1.0f), outDir.getChildFile ("tab" + juce::String (tab) + "-all.png")))
                        return 1;
        }
    }
    else if (! writePng (snapshot(), outDir.getChildFile ("editor.png")))
    {
        return 1;
    }

    // An overlay opened by a button with this component ID (damp's tuner) gets a picture too
    if (auto* button = dynamic_cast<juce::Button*> (findById (*editor, "tunerButton")))
    {
        button->setToggleState (! button->getToggleState(), juce::dontSendNotification);
        if (button->onClick)
            button->onClick(); // as a click would, but now rather than via the message queue
        if (! writePng (snapshot(), outDir.getChildFile ("tuner.png")))
            return 1;
    }

    processor->editorBeingDeleted (editor.get());
    editor.reset(); // editor before processor
    return 0;
}
