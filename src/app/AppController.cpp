#include "app/AppController.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <thread>

#include "app/model/Edit.h"
#include "app/ui/ExportDialog.h"

namespace ddaw::app {

AppController::AppController(AppModel& model, ui::MainComponent& main, AudioHost* host, std::function<void()> quit)
    : model_(model), main_(main), host_(host), quit_(std::move(quit)) {
    commands_.registerAllCommandsForTarget(this);
    commands_.setFirstCommandTarget(this);
    main_.addKeyListener(commands_.getKeyMappings());
    main_.onAudioSettings = [this] { audioSettings(); };
    main_.browser().onImportSample = [this] { importSample(); };
    main_.browser().onScanPlugins = [this] { scanPlugins(); };
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#endif
}

AppController::~AppController() {
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    main_.removeKeyListener(commands_.getKeyMappings());
    if (exportWindow_) exportWindow_.deleteAndZero();   // closing the dialog cancels a running export
}

void AppController::getAllCommands(juce::Array<juce::CommandID>& c) {
    c.addArray({NewProject, OpenProject, SaveProject, SaveProjectAs, ExportAudio, Quit, Undo, Redo, DeleteSelection, ShowSession, ShowArrangement,
                ShowMixer, ShowClip, ShowDevices, AudioSettings, AddSynthTrack, AddDrumTrack, AddAudioTrack, AddBusTrack, AddScene, PlayStop,
                ToggleMetronome, ImportSample});
}

void AppController::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& i) {
    using M = juce::ModifierKeys;
    const auto cmd = M::commandModifier, shift = M::shiftModifier;
    auto set = [&](const char* name, const char* cat, juce::KeyPress key = {}) {
        i.setInfo(name, name, cat, 0);
        if (key.isValid()) i.addDefaultKeypress(key.getKeyCode(), key.getModifiers());
    };
    switch (id) {
        case NewProject: set("New Project", "File", {'n', cmd, 0}); break;
        case OpenProject: set("Open...", "File", {'o', cmd, 0}); break;
        case SaveProject: set("Save", "File", {'s', cmd, 0}); break;
        case SaveProjectAs: set("Save As...", "File", {'s', cmd | shift, 0}); break;
        case ExportAudio: set("Export Audio...", "File", {'e', cmd | shift, 0}); break;
        case Quit: set("Quit", "File", {'q', cmd, 0}); break;
        case Undo: set("Undo", "Edit", {'z', cmd, 0}); i.setActive(model_.canUndo()); break;
        case Redo: set("Redo", "Edit", {'z', cmd | shift, 0}); i.setActive(model_.canRedo()); break;
        case DeleteSelection: set("Delete", "Edit"); break;
        case ShowSession: set("Session", "View", {'1', cmd, 0}); break;
        case ShowArrangement: set("Arrangement", "View", {'2', cmd, 0}); break;
        case ShowMixer: set("Mixer", "View", {'3', cmd, 0}); break;
        case ShowClip: set("Clip Editor", "View", {'4', cmd, 0}); break;
        case ShowDevices: set("Devices", "View", {'5', cmd, 0}); break;
        case AudioSettings: set("Audio Settings...", "View", {',', cmd, 0}); break;
        case AddSynthTrack: set("Add Synth Track", "Track", {'t', cmd, 0}); break;
        case AddDrumTrack: set("Add Drum Track", "Track", {'t', cmd | shift, 0}); break;
        case AddAudioTrack: set("Add Audio Track", "Track"); break;
        case AddBusTrack: set("Add Bus", "Track"); break;
        case AddScene: set("Add Scene", "Track", {'i', cmd | shift, 0}); break;
        case PlayStop: set("Play / Stop", "Transport"); break;
        case ToggleMetronome: set("Metronome", "Transport"); i.setTicked(model_.metronome()); break;
        case ImportSample: set("Import Sample...", "File", {'i', cmd, 0}); break;
        default: break;
    }
}

juce::PopupMenu AppController::getMenuForIndex(int index, const juce::String&) {
    juce::PopupMenu m;
    auto item = [&](Command c) { m.addCommandItem(&commands_, c); };
    switch (index) {
        case 0: item(NewProject); item(OpenProject); m.addSeparator(); item(SaveProject); item(SaveProjectAs); m.addSeparator(); item(ImportSample); item(ExportAudio);
#if !JUCE_MAC
            m.addSeparator(); item(Quit);
#endif
            break;
        case 1: item(Undo); item(Redo); break;
        case 2: item(AddSynthTrack); item(AddDrumTrack); item(AddAudioTrack); item(AddBusTrack); m.addSeparator(); item(AddScene); break;
        case 3: item(ShowSession); item(ShowArrangement); item(ShowMixer); m.addSeparator(); item(ShowClip); item(ShowDevices); m.addSeparator(); item(AudioSettings); break;
        case 4: item(PlayStop); item(ToggleMetronome); break;
        default: break;
    }
    return m;
}

bool AppController::perform(const InvocationInfo& info) {
    using K = project::TrackKind;
    auto addTrack = [&](K k) { model_.apply(edit::addTrack(model_.project(), k)); model_.selectTrack(model_.project().tracks.back().uid); };
    switch (info.commandID) {
        case NewProject: confirmDiscard([this] { newProject(); }); break;
        case OpenProject: confirmDiscard([this] { openProject(); }); break;
        case SaveProject: save(); break;
        case SaveProjectAs: saveAs(); break;
        case ExportAudio: exportAudio(); break;
        case Quit: confirmDiscard(quit_); break;
        case Undo: model_.undo(); break;
        case Redo: model_.redo(); break;
        case ShowSession: main_.showMain(ui::MainComponent::MainTab::Session); model_.setArrangementMode(false); model_.notify(ModelEvent::Transport); break;
        case ShowArrangement: main_.showMain(ui::MainComponent::MainTab::Arrangement); model_.setArrangementMode(true); model_.notify(ModelEvent::Transport); break;
        case ShowMixer: main_.showMain(ui::MainComponent::MainTab::Mixer); break;
        case ShowClip: main_.showDetail(ui::MainComponent::DetailTab::Clip); break;
        case ShowDevices: main_.showDetail(ui::MainComponent::DetailTab::Devices); break;
        case AudioSettings: audioSettings(); break;
        case AddSynthTrack: addTrack(K::Synth); break;
        case AddDrumTrack: addTrack(K::Drum); break;
        case AddAudioTrack: addTrack(K::Audio); break;
        case AddBusTrack: addTrack(K::Bus); break;
        case AddScene: model_.apply(edit::addScene(model_.project())); break;
        case PlayStop: model_.togglePlay(); break;
        case ToggleMetronome: model_.setMetronome(!model_.metronome()); break;
        case ImportSample: importSample(); break;
        default: return false;
    }
    commands_.commandStatusChanged();
    return true;
}

void AppController::confirmDiscard(std::function<void()> proceed) {
    if (!model_.dirty()) { proceed(); return; }
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon).withTitle("Unsaved changes")
            .withMessage("Save changes to \"" + juce::String(model_.title()).trimCharactersAtEnd(" *") + "\" before continuing?")
            .withButton("Save").withButton("Don't Save").withButton("Cancel"),
        [this, next = std::move(proceed)](int r) {
            if (r == 1) save(next);             // Save
            else if (r == 2) next();            // Don't Save
        });
}

void AppController::newProject() { model_.newProject(); setStatus("New project"); }

void AppController::openProject() {
    chooser_ = std::make_unique<juce::FileChooser>("Open a project (a .ddaw package or a synthyy .json)", juce::File(), "*.json;*.ddaw");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              std::string err;
                              if (model_.open(f.getFullPathName().toStdString(), err)) setStatus("Opened " + f.getFileName());
                              else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot open project", err);
                          });
}

void AppController::save(std::function<void()> then) {
    if (model_.path().empty()) { saveAs(std::move(then)); return; }
    std::string err;
    if (model_.save(model_.path(), err)) { setStatus("Saved " + juce::String(model_.path())); if (then) then(); }
    else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot save project", err);
}

void AppController::saveAs(std::function<void()> then) {
    chooser_ = std::make_unique<juce::FileChooser>("Save project as", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Untitled.ddaw"), "*.ddaw");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, after = std::move(then)](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (f.getFileExtension().isEmpty()) f = f.withFileExtension("ddaw");
                              std::string err;
                              if (model_.save(f.getFullPathName().toStdString(), err)) { setStatus("Saved " + f.getFullPathName()); if (after) after(); }
                              else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot save project", err);
                          });
}

void AppController::importSample() {
    chooser_ = std::make_unique<juce::FileChooser>("Import samples", juce::File(), "*.wav");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this](const juce::FileChooser& fc) {
                              int ok = 0;
                              for (const auto& f : fc.getResults()) {
                                  std::string id, err;
                                  if (model_.loadWavSample(f.getFullPathName().toStdString(), id, err)) ++ok;
                                  else setStatus("Cannot import " + f.getFileName() + ": " + err);
                              }
                              if (ok) { setStatus("Imported " + juce::String(ok) + " sample(s)"); model_.notify(ModelEvent::File); }
                          });
}

void AppController::scanPlugins() {
    auto* pp = model_.pluginProvider();
    if (!pp) return;
    setStatus("Scanning for plugins...");
    // let the status paint first: the scan loads every plugin on this thread and takes a few seconds
    juce::Timer::callAfterDelay(60, [this, pp] {
        pp->scan([this](const std::string& name) { setStatus("Scanning: " + juce::String(name)); });
        setStatus(juce::String(int(pp->available().size())) + " plugin(s) found");
        model_.notify(ModelEvent::File);
    });
}

void AppController::audioSettings() {
    if (!host_) return;
    auto* sel = new juce::AudioDeviceSelectorComponent(host_->deviceManager(), 0, 0, 0, 2, false, false, true, false);
    sel->setSize(520, 380);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(sel);
    o.dialogTitle = "Audio Settings";
    o.componentToCentreAround = &main_;
    o.dialogBackgroundColour = ui::col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void AppController::exportAudio() {
    if (exportWindow_) return;   // one at a time
    auto* dlg = new ui::ExportDialog(model_, [this](const juce::String& msg) {
        if (msg.isNotEmpty()) setStatus(msg);
        juce::MessageManager::callAsync([this] { if (exportWindow_) exportWindow_->exitModalState(0); });
    });
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(dlg);
    o.dialogTitle = "Export";
    o.componentToCentreAround = &main_;
    o.dialogBackgroundColour = ui::col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    exportWindow_ = o.launchAsync();
}

}  // namespace ddaw::app
