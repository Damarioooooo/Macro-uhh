#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
struct Event { int tick = 0; bool down = false; int button = 1; bool player1 = true; };
struct Sample { int percent = 0; int tick = 0; float x = 0.f; float y = 0.f; };
struct MacroData { std::vector<Event> events; Sample start{}, end{}; bool valid = false; };
MacroData g_macro;
int g_tick = 0;
bool g_recording = false, g_playing = false, g_forwarding = false;
size_t g_eventIndex = 0;
int g_playStartTick = 0;

bool settingBool(char const* key, bool fallback = false) {
    auto mod = Mod::get();
    return mod ? mod->getSettingValue<bool>(key) : fallback;
}
int percent(PlayLayer* layer) {
    return layer ? std::clamp(static_cast<int>(layer->getCurrentPercent()), 0, 100) : 0;
}
PlayerObject* player(PlayLayer* layer) { return layer ? layer->m_player1 : nullptr; }
Sample sampleNow(PlayLayer* layer) {
    Sample s; s.percent = percent(layer); s.tick = g_tick;
    if (auto p = player(layer)) { s.x = p->getPositionX(); s.y = p->getPositionY(); }
    return s;
}
std::filesystem::path macroPath() { return Mod::get()->getSaveDir() / "segment-macro.txt"; }
void saveMacro() {
    std::ofstream out(macroPath());
    if (!out) { log::error("Segment Macro: couldn't save recording"); return; }
    out << "SGM2\n" << g_macro.start.percent << ' ' << g_macro.start.tick << ' ' << g_macro.start.x << ' ' << g_macro.start.y << '\n';
    out << g_macro.end.percent << ' ' << g_macro.end.tick << ' ' << g_macro.end.x << ' ' << g_macro.end.y << '\n';
    out << g_macro.events.size() << '\n';
    for (auto const& e : g_macro.events) out << e.tick << ' ' << e.down << ' ' << e.button << ' ' << e.player1 << '\n';
}
bool loadMacro() {
    std::ifstream in(macroPath()); std::string magic; size_t count = 0; MacroData loaded;
    if (!(in >> magic) || magic != "SGM2") return false;
    if (!(in >> loaded.start.percent >> loaded.start.tick >> loaded.start.x >> loaded.start.y)) return false;
    if (!(in >> loaded.end.percent >> loaded.end.tick >> loaded.end.x >> loaded.end.y)) return false;
    if (!(in >> count) || count > 1000000) return false;
    loaded.events.reserve(count);
    for (size_t i=0; i<count; ++i) { Event e; if (!(in >> e.tick >> e.down >> e.button >> e.player1)) return false; loaded.events.push_back(e); }
    loaded.valid = !loaded.events.empty(); g_macro = std::move(loaded); return g_macro.valid;
}
void stopPlayback() { g_playing = false; g_eventIndex = 0; }
void beginRecord(PlayLayer* layer) {
    if (!layer) return;
    g_macro = MacroData{}; g_macro.start = sampleNow(layer); g_recording = true; stopPlayback();
    log::info("Segment Macro: recording started at captured {}%", g_macro.start.percent);
}
void endRecord(PlayLayer* layer) {
    if (!g_recording || !layer) return;
    g_macro.end = sampleNow(layer); g_recording = false;
    g_macro.valid = !g_macro.events.empty() && g_macro.end.tick >= g_macro.start.tick;
    if (g_macro.valid) { saveMacro(); log::info("Segment Macro: saved {} transitions, {}% to {}%", g_macro.events.size(), g_macro.start.percent, g_macro.end.percent); }
    else log::warn("Segment Macro: recording contained no button transitions");
}
bool nearStart(PlayLayer* layer) {
    if (!g_macro.valid || !player(layer)) return false;
    // Percentage is only a coarse trigger; position and a small tolerance reduce false starts.
    if (percent(layer) != g_macro.start.percent) return false;
    auto p = player(layer);
    constexpr float tolerance = 3.0f;
    return std::fabs(p->getPositionX()-g_macro.start.x) <= tolerance && std::fabs(p->getPositionY()-g_macro.start.y) <= tolerance;
}
bool nearEnd(PlayLayer* layer) {
    if (!g_macro.valid || !player(layer)) return true;
    auto p = player(layer);
    return percent(layer) >= g_macro.end.percent && std::fabs(p->getPositionX()-g_macro.end.x) <= 8.0f;
}
void tickMacro(PlayLayer* layer) {
    if (!layer) return;
    ++g_tick;
    if (!g_recording && !g_playing && settingBool("auto-playback") && g_macro.valid && nearStart(layer)) {
        g_playing = true; g_eventIndex = 0; g_playStartTick = g_tick;
        log::info("Segment Macro: matched recorded start state; playback takeover");
    }
    if (g_playing) {
        int elapsed = g_tick - g_playStartTick;
        while (g_eventIndex < g_macro.events.size() && g_macro.events[g_eventIndex].tick <= elapsed) {
            auto e = g_macro.events[g_eventIndex++];
            g_forwarding = true; layer->handleButton(e.down, e.button, e.player1); g_forwarding = false;
        }
        // Stop at the recorded end position (or after all recorded transitions and end progress).
        if (nearEnd(layer) || (g_eventIndex >= g_macro.events.size() && percent(layer) >= g_macro.end.percent)) {
            stopPlayback(); log::info("Segment Macro: endpoint reached; real inputs resumed");
        }
    }
}
}

class $modify(SegmentMacroPlayLayer, PlayLayer) {
    struct Fields { CCMenu* macroMenu = nullptr; CCMenuItemLabel* macroButton = nullptr; };
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_tick = 0; g_recording = false; stopPlayback();
        if (!g_macro.valid) loadMacro();
        if (!settingBool("hide-floating-button")) {
            auto label = CCLabelBMFont::create("MACRO", "bigFont.fnt");
            if (label) {
                m_fields->macroButton = CCMenuItemLabel::create(label, this, menu_selector(SegmentMacroPlayLayer::onMacroButton));
                m_fields->macroMenu = CCMenu::create();
                if (m_fields->macroButton && m_fields->macroMenu) {
                    m_fields->macroButton->setScale(0.45f);
                    m_fields->macroMenu->addChild(m_fields->macroButton);
                    m_fields->macroMenu->setPosition({0,0});
                    auto win = CCDirector::sharedDirector()->getWinSize();
                    m_fields->macroButton->setPosition({win.width - 48.f, win.height * 0.55f});
                    this->addChild(m_fields->macroMenu, 9999);
                }
            }
        }
        return true;
    }
    void onMacroButton(CCObject*) {
        if (g_recording) endRecord(this);
        else if (g_macro.valid) {
            if (g_playing) stopPlayback();
            else { Mod::get()->setSettingValue("auto-playback", true); log::info("Segment Macro: auto-playback armed; begin a run matching the saved start"); }
        } else {
            // Eclipse-style recording is intended for Practice Mode. Refuse to record outside practice.
            if (!m_isPracticeMode) { FLAlertLayer::create("Segment Macro", "Start Practice Mode first, then tap MACRO to begin recording. Tap again to save the segment.", "OK")->show(); return; }
            beginRecord(this);
        }
    }
    void onQuit() { g_recording = false; stopPlayback(); PlayLayer::onQuit(); }
};

class $modify(SegmentMacroBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto layer = PlayLayer::get();
        if (!g_forwarding && layer && button == 1 && isPlayer1) {
            if (g_recording) g_macro.events.push_back({g_tick - g_macro.start.tick, down, button, isPlayer1});
            if (g_playing) return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(SegmentMacroPlayerObject, PlayerObject) {
    void update(float dt) {
        auto layer = PlayLayer::get();
        if (layer && this == layer->m_player1) tickMacro(layer);
        PlayerObject::update(dt);
    }
};
