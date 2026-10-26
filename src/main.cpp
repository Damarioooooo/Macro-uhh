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

// Experimental starter implementation for GD 2.2081 / Geode 5.11.2.
// Percentage-triggered input replay is not equivalent to deterministic replay:
// state comparison below is only a conservative position guard. Runtime/API
// compatibility and touch-indicator behavior must be verified on an iOS device.
namespace {
struct Event { int tick = 0; bool down = false; int button = 1; bool player1 = true; };
struct Sample { int percent = 0; int tick = 0; float x = 0.f; float y = 0.f; };
struct MacroData {
    std::vector<Event> events;
    Sample start{};
    Sample end{};
    bool valid = false;
};

MacroData g_macro;
int g_tick = 0;
bool g_recording = false;
bool g_playing = false;
bool g_forwarding = false;
size_t g_eventIndex = 0;
int g_playStartTick = 0;
bool g_prevRecordSetting = false;
bool g_prevAutoSetting = false;

int settingInt(char const* key, int fallback) {
    auto mod = Mod::get();
    if (!mod) return fallback;
    return static_cast<int>(mod->getSettingValue<int64_t>(key));
}
bool settingBool(char const* key, bool fallback = false) {
    auto mod = Mod::get();
    if (!mod) return fallback;
    return mod->getSettingValue<bool>(key);
}
int percent(PlayLayer* layer) {
    if (!layer) return 0;
    return std::clamp(static_cast<int>(layer->getCurrentPercent()), 0, 100);
}
PlayerObject* player(PlayLayer* layer) { return layer ? layer->m_player1 : nullptr; }
Sample sampleNow(PlayLayer* layer) {
    Sample s;
    s.percent = percent(layer);
    s.tick = g_tick;
    if (auto p = player(layer)) { s.x = p->getPositionX(); s.y = p->getPositionY(); }
    return s;
}
std::filesystem::path macroPath() {
    auto mod = Mod::get();
    return mod->getSaveDir() / "segment-macro.txt";
}
void saveMacro() {
    std::ofstream out(macroPath());
    if (!out) { log::error("Segment Macro: failed to save macro"); return; }
    out << "SGM1\n" << g_macro.start.percent << ' ' << g_macro.start.tick << ' ' << g_macro.start.x << ' ' << g_macro.start.y << '\n';
    out << g_macro.end.percent << ' ' << g_macro.end.tick << ' ' << g_macro.end.x << ' ' << g_macro.end.y << '\n';
    out << g_macro.events.size() << '\n';
    for (auto const& e : g_macro.events) out << e.tick << ' ' << e.down << ' ' << e.button << ' ' << e.player1 << '\n';
}
bool loadMacro() {
    std::ifstream in(macroPath());
    std::string magic;
    size_t count = 0;
    if (!(in >> magic) || magic != "SGM1") return false;
    MacroData loaded;
    if (!(in >> loaded.start.percent >> loaded.start.tick >> loaded.start.x >> loaded.start.y)) return false;
    if (!(in >> loaded.end.percent >> loaded.end.tick >> loaded.end.x >> loaded.end.y)) return false;
    if (!(in >> count) || count > 1000000) return false;
    loaded.events.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Event e;
        if (!(in >> e.tick >> e.down >> e.button >> e.player1)) return false;
        loaded.events.push_back(e);
    }
    loaded.valid = true;
    g_macro = std::move(loaded);
    return true;
}
void resetRun() {
    g_tick = 0; g_recording = false; g_playing = false; g_forwarding = false;
    g_eventIndex = 0; g_playStartTick = 0;
}
void beginRecord(PlayLayer* layer) {
    g_macro = MacroData{};
    g_macro.start = sampleNow(layer);
    g_recording = true;
    log::info("Segment Macro: recording armed at {}%", g_macro.start.percent);
}
void endRecord(PlayLayer* layer) {
    g_macro.end = sampleNow(layer);
    g_macro.valid = !g_macro.events.empty();
    g_recording = false;
    if (g_macro.valid) { saveMacro(); log::info("Segment Macro: saved {} input transitions", g_macro.events.size()); }
    else log::warn("Segment Macro: no inputs recorded; macro not saved");
}
bool startStateMatches(PlayLayer* layer) {
    if (!g_macro.valid || !player(layer)) return false;
    auto p = player(layer);
    float tolerance = static_cast<float>(settingInt("state-tolerance", 4));
    // Position-only is a guard, not a complete deterministic state check.
    return std::fabs(p->getPositionX() - g_macro.start.x) <= tolerance &&
           std::fabs(p->getPositionY() - g_macro.start.y) <= tolerance;
}
void tickMacro(PlayLayer* layer) {
    if (!layer) return;
    ++g_tick;
    int pct = percent(layer);
    int startPct = settingInt("start-percent", 50);
    int endPct = std::max(startPct + 1, settingInt("end-percent", 75));

    bool recordSetting = settingBool("record-segment");
    if (recordSetting && !g_prevRecordSetting) {
        g_macro = MacroData{};
        g_recording = false; // begin exactly when progress reaches configured start
    }
    if (recordSetting && !g_recording && pct >= startPct && pct < endPct) beginRecord(layer);
    if (g_recording && pct >= endPct) endRecord(layer);
    if (!recordSetting && g_recording) endRecord(layer);
    g_prevRecordSetting = recordSetting;

    bool autoSetting = settingBool("auto-takeover");
    if (autoSetting && !g_prevAutoSetting) {
        if (!g_macro.valid) loadMacro();
        g_playing = false; g_eventIndex = 0;
    }
    g_prevAutoSetting = autoSetting;

    if (autoSetting && !g_playing && g_macro.valid && pct >= startPct && pct < endPct) {
        if (startStateMatches(layer)) {
            g_playing = true; g_eventIndex = 0; g_playStartTick = g_tick;
            log::info("Segment Macro: automatic takeover started");
        }
    }
    if (g_playing) {
        int elapsed = g_tick - g_playStartTick;
        while (g_eventIndex < g_macro.events.size() && g_macro.events[g_eventIndex].tick <= elapsed) {
            auto const e = g_macro.events[g_eventIndex++];
            g_forwarding = true;
            layer->handleButton(e.down, e.button, e.player1);
            g_forwarding = false;
        }
        if (pct >= endPct || g_eventIndex >= g_macro.events.size()) {
            g_playing = false;
            log::info("Segment Macro: playback finished");
        }
    }
}
}

class $modify(SegmentMacroPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        resetRun();
        if (!g_macro.valid) loadMacro();
        return true;
    }
    void onQuit() {
        resetRun();
        PlayLayer::onQuit();
    }
};

class $modify(SegmentMacroBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto layer = PlayLayer::get();
        if (!g_forwarding && layer && button == 1 && isPlayer1) {
            if (g_recording) {
                g_macro.events.push_back({g_tick - g_macro.start.tick, down, button, isPlayer1});
            }
            if (g_playing) return; // recorded inputs own the primary-player input during takeover
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
