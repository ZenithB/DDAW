// Controller bindings (B5): a gamepad axis or a MIDI CC drives a morph stick or a macro, live, without filling the undo
// stack; "learn" binds the next control that moves; bindings are saved with the project.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/model/AppModel.h"
#include "app/model/Controllers.h"
#include "project/ProjectJson.h"

using namespace ddaw;
using namespace ddaw::app;
using Catch::Approx;

namespace {
struct Rig {
    engine::Engine eng;
    std::unique_ptr<AppModel> m;
    project::Uid track = 0;
    std::string id;
    Rig() {
        eng.prepare(48000.0);
        m = std::make_unique<AppModel>(eng, 48000.0);
        REQUIRE(m->apply(edit::addTrack(m->project(), project::TrackKind::Synth)));
        track = m->project().tracks[0].uid;
        id = m->project().tracks[0].id;
        project::MorphSpec map;
        map.name = "m"; map.targets = {{"inst", "inst", "cutoff"}};
        map.anchors = {{"lo", 0.1, 0.5, {0.1}}, {"hi", 0.9, 0.5, {0.9}}};
        REQUIRE(m->apply({"morph.insert", {{"track", track}, {"index", 0}, {"morph", project::morphToJson(map)}}}));
        project::MacroSpec mac; mac.name = "mac"; mac.targets = {{"inst", "inst", "cutoff"}};
        REQUIRE(m->apply({"macro.insert", {{"track", track}, {"index", 0}, {"macro", project::macroToJson(mac)}}}));
    }
    void bind(const std::string& source, const std::string& target, double lo = 0, double hi = 1, bool invert = false) {
        project::ControlBinding b; b.source = source; b.target = target; b.min = lo; b.max = hi; b.invert = invert;
        REQUIRE(m->apply({"binding.insert", {{"index", m->project().bindings.size()}, {"binding", project::bindingToJson(b)}}}));
    }
    const project::MorphSpec& morph() const { return m->project().tracks[0].morph[0]; }
};
}  // namespace

TEST_CASE("controllers: a bound axis moves the morph stick, and is not an undo step", "[controllers]") {
    Rig r;
    r.bind("pad:lx", morphTarget(r.id, 0, 'x'));
    r.bind("pad:ly", morphTarget(r.id, 0, 'y'));
    REQUIRE(r.m->apply(document::cmd::setTrack(r.track, "name", std::string("renamed"))));   // the one real undo step
    CHECK(r.m->controllerInput("pad:lx", 0.8));
    CHECK(r.m->controllerInput("pad:ly", 0.25));
    CHECK(r.morph().x == Approx(0.8));
    CHECK(r.morph().y == Approx(0.25));
    for (int i = 0; i < 100; ++i) r.m->controllerInput("pad:lx", double(i) / 100.0);   // a stick sweep
    CHECK(r.morph().x == Approx(0.99));
    r.m->undo();
    CHECK(r.m->project().tracks[0].name != "renamed");   // undo reverted the rename, not the stick motion
    CHECK(r.morph().x == Approx(0.99));
    CHECK_FALSE(r.m->controllerInput("pad:rx", 0.5));      // nothing bound to it
}

TEST_CASE("controllers: range and invert map the input onto the target's own scale", "[controllers]") {
    Rig r;
    r.bind("midi:cc74", morphTarget(r.id, 0, 'x'), 0.25, 0.75, true);
    r.m->controllerInput("midi:cc74", 1.0);
    CHECK(r.morph().x == Approx(0.25));
    r.m->controllerInput("midi:cc74", 0.0);
    CHECK(r.morph().x == Approx(0.75));
    r.m->controllerInput("midi:cc74", 0.5);
    CHECK(r.morph().x == Approx(0.5));
}

TEST_CASE("controllers: a macro is a target too", "[controllers]") {
    Rig r;
    r.bind("pad:rt", macroTarget(r.id, 0));
    r.m->controllerInput("pad:rt", 0.7);
    CHECK(r.m->project().tracks[0].macros[0].value == Approx(0.7));
    // a binding to something that no longer exists is ignored, not an error
    r.bind("pad:a", macroTarget("gone", 3));
    r.bind("pad:b", morphTarget(r.id, 9, 'x'));
    CHECK_NOTHROW(r.m->controllerInput("pad:a", 1.0));
    CHECK_NOTHROW(r.m->controllerInput("pad:b", 1.0));
}

TEST_CASE("controllers: learn binds the first control that moves clearly, and replaces the old binding of that target", "[controllers]") {
    Rig r;
    r.bind("pad:lx", morphTarget(r.id, 0, 'x'));
    r.m->startLearn(morphTarget(r.id, 0, 'x'));
    CHECK(r.m->learnTarget() == morphTarget(r.id, 0, 'x'));
    CHECK(r.m->controllerInput("pad:ry", 0.5));      // the first value of a source is only a baseline
    CHECK(r.m->controllerInput("pad:ry", 0.55));     // a wobble is not a gesture
    CHECK(r.m->learnTarget() != "");
    CHECK(r.m->controllerInput("pad:ry", 0.95));     // a clear move
    CHECK(r.m->learnTarget() == "");
    REQUIRE(r.m->project().bindings.size() == 1);    // pad:lx's binding of this target was replaced
    CHECK(r.m->project().bindings[0].source == "pad:ry");
    r.m->controllerInput("pad:ry", 0.1);
    CHECK(r.morph().x == Approx(0.1));
    r.m->controllerInput("pad:lx", 0.9);             // the old source no longer does anything
    CHECK(r.morph().x == Approx(0.1));

    r.m->startLearn(macroTarget(r.id, 0));
    r.m->cancelLearn();
    CHECK(r.m->learnTarget() == "");
    CHECK(r.m->project().bindings.size() == 1);
}

TEST_CASE("controllers: bindings are saved with the project and undoable", "[controllers]") {
    Rig r;
    r.bind("pad:lx", morphTarget(r.id, 0, 'x'), 0.2, 0.9, true);
    const auto j = project::projectToJson(r.m->project());
    REQUIRE(j.contains("bindings"));
    const auto back = project::projectFromJson(j);
    REQUIRE(back.bindings.size() == 1);
    CHECK(back.bindings[0].source == "pad:lx");
    CHECK(back.bindings[0].target == morphTarget(r.id, 0, 'x'));
    CHECK(back.bindings[0].min == Approx(0.2));
    CHECK(back.bindings[0].max == Approx(0.9));
    CHECK(back.bindings[0].invert);
    r.m->undo();
    CHECK(r.m->project().bindings.empty());
    CHECK(describeSource("pad:ly") == "Left stick Y");
    CHECK(describeSource("midi:cc74") == "MIDI CC 74");
}
