#pragma once
// Window-level commands: the menu bar, keyboard shortcuts, file dialogs, export and the unsaved-changes
// guard. Lives with the application (it needs JUCE file choosers and the audio formats).
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AudioHost.h"
#include "app/ui/ExportDialog.h"
#include "app/ui/MainComponent.h"

namespace ddaw::app {

class AppController : public juce::ApplicationCommandTarget, public juce::MenuBarModel {
public:
    enum Command : int {
        NewProject = 0x2000, OpenProject, SaveProject, SaveProjectAs, ExportAudio, Quit,
        Undo, Redo, DeleteSelection,
        ShowSession, ShowArrangement, ShowMixer, ShowClip, ShowDevices, AudioSettings,
        AddSynthTrack, AddDrumTrack, AddAudioTrack, AddBusTrack, AddScene,
        PlayStop, ToggleMetronome, ImportSample
    };
    AppController(AppModel& model, ui::MainComponent& main, AudioHost* host, std::function<void()> quit);
    ~AppController() override;

    // The window asks before it closes; `proceed` runs once unsaved work is saved or discarded.
    void confirmDiscard(std::function<void()> proceed);

    // MenuBarModel
    juce::StringArray getMenuBarNames() override { return {"File", "Edit", "Track", "View", "Transport"}; }
    juce::PopupMenu getMenuForIndex(int, const juce::String&) override;
    void menuItemSelected(int, int) override {}

    // ApplicationCommandTarget
    ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>&) override;
    void getCommandInfo(juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform(const InvocationInfo&) override;

    juce::ApplicationCommandManager& commands() { return commands_; }

    void newProject();
    void openProject();
    void save(std::function<void()> then = {});
    void saveAs(std::function<void()> then = {});
    void exportAudio();
    void importSample();
    void scanPlugins();
    void audioSettings();

private:
    void setStatus(const juce::String& s) { main_.setStatus(s); }

    AppModel& model_;
    ui::MainComponent& main_;
    AudioHost* host_;
    std::function<void()> quit_;
    juce::ApplicationCommandManager commands_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::DialogWindow> audioDialog_;
    juce::Component::SafePointer<juce::Component> exportWindow_;
};

}  // namespace ddaw::app
