#include "app/ui/ParamPicker.h"

#include "app/model/Catalog.h"

namespace ddaw::ui {

namespace {
const ParamSpec kGain{0, "gain", -48.0f, 6.0f, 0.0f, Curve::Linear, 0, false};
const ParamSpec kPan{0, "pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, false};
}  // namespace

bool targetSpec(const project::Track& t, const project::ModTarget& m, ParamSpec& out, double& stored) {
    auto fromDevice = [&](const project::DeviceSpec& d, app::Chain chain) {
        const auto* info = app::findDevice(chain, d.type);
        if (!info) return false;
        for (const auto& ps : info->params)
            if (m.pkey == ps.key) {
                out = ps;
                auto it = d.params.find(m.pkey);
                stored = it != d.params.end() ? it->second : double(ps.def);
                return true;
            }
        return false;
    };
    if (m.dest == "inst") return fromDevice(t.inst, app::Chain::Instrument);
    if (m.dest == "mix") {
        if (m.pkey == "gain") { out = kGain; stored = t.gainDb; return true; }
        if (m.pkey == "pan") { out = kPan; stored = t.pan; return true; }
        return false;
    }
    for (const auto& d : t.fx) if (d.id == m.fxId) return fromDevice(d, app::Chain::Effect);
    return false;
}

juce::String describeTarget(const project::Track& t, const project::ModTarget& m) {
    if (m.dest == "mix") return m.pkey == "gain" ? "Volume" : m.pkey == "pan" ? "Pan" : juce::String(m.pkey);
    const juce::String p = app::paramLabel(m.pkey);
    if (m.dest == "inst") return p;
    juce::String dev = m.fxId;
    for (const auto& d : t.fx) if (d.id == m.fxId) { if (const auto* info = app::findDevice(app::Chain::Effect, d.type)) dev = info->label; }
    return dev + " " + p;
}

void pickParam(app::AppModel& model, project::Uid track, juce::Component* anchor, std::function<void(const PickedParam&)> done) {
    const auto* t = app::edit::findTrack(model.project(), track);
    if (!t) return;
    auto choices = std::make_shared<std::vector<PickedParam>>();
    juce::PopupMenu menu;
    int id = 1;
    auto addDevice = [&](const juce::String& title, app::Chain chain, const project::DeviceSpec& d, const std::string& dest, const std::string& fxId) {
        const auto* info = app::findDevice(chain, d.type);
        if (!info || info->params.empty()) return;
        juce::PopupMenu sub;
        for (const auto& ps : info->params) {
            auto it = d.params.find(ps.key);
            PickedParam pp{dest, fxId, ps.key, app::paramLabel(ps.key) + " (" + info->label + ")", ps, it != d.params.end() ? it->second : double(ps.def)};
            sub.addItem(id++, app::paramLabel(ps.key));
            choices->push_back(pp);
        }
        menu.addSubMenu(title, sub);
    };
    if (!t->inst.type.empty() && t->kind != project::TrackKind::Bus) addDevice("Instrument: " + juce::String(t->inst.type), app::Chain::Instrument, t->inst, "inst", "inst");
    for (const auto& d : t->fx) addDevice(juce::String(d.id), app::Chain::Effect, d, "fx", d.id);
    juce::PopupMenu mix;
    mix.addItem(id++, "Volume"); choices->push_back({"mix", "", "gain", "Volume", kGain, t->gainDb});
    mix.addItem(id++, "Pan"); choices->push_back({"mix", "", "pan", "Pan", kPan, t->pan});
    menu.addSubMenu("Mixer", mix);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor), [choices, done = std::move(done)](int r) {
        if (r >= 1 && size_t(r) <= choices->size() && done) done((*choices)[size_t(r - 1)]);
    });
}

}  // namespace ddaw::ui
