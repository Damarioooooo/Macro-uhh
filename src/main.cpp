
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
struct Event {
    int tick = 0;
    bool down = false;
    int button = 1;
    bool player1 = true;
};

struct Sample {
    int percent = 0;
    int tick = 0;
    float x = 0.f;
    float y = 0.f;
};

struct MacroData {
    std::vector<Event> events;
    Sample start{}, end{};
    bool valid = false;
};

MacroData g_macro;
int g_tick = 0;
bool g_recording = false;
bool g_playing = false;
bool g_forwarding = false;
size_t g_eventIndex = 0;
int g_playStartTick = 0;

bool settingBool(char const* key, bool fallback = false) {
    auto mod = Mod::get();
    return mod ? mod->getSettingValue<bool>(key) : fallback;
}

int percent(PlayLayer* layer) {
    return layer
        ? std::clamp(static_cast<int>(layer->getCurrentPercent()), 0, 100)
        : 0;
}

PlayerObject* player(PlayLayer* layer) {
    return layer ? layer->m_player1 : nullptr;
}

Sample sampleNow(PlayLayer* layer) {
    Sample s;
    s.percent = percent(layer);
    s.tick = g_tick;

    if (auto p = player(layer)) {
        s.x = p->getPositionX();
        s.y = p->getPositionY();
    }

    return s;
}

std::filesystem::path macroPath() {
    return Mod::get()->getSaveDir() / "segment-macro.txt";
}

void saveMacro() {
    std::ofstream out(macroPath());

    if (!out) {
        log::error("Segment Macro: couldn't save recording");
        return;
    }

    out << "SGM2\n"
        << g_macro.start.percent << ' '
        << g_macro.start.tick << ' '
        << g_macro.start.x << ' '
        << g_macro.start.y << '\n';

    out << g_macro.end.percent << ' '
        << g_macro.end.tick << ' '
        << g_macro.end.x << ' '
        << g_macro.end.y << '\n';

    out << g_macro.events.size() << '\n';

    for (auto const& e : g_macro.events) {
        out << e.tick << ' '
            << e.down << ' '
            << e.button << ' '
            << e.player1 << '\n';
    }
}

bool loadMacro() {
    std::ifstream in(macroPath());
    std::string magic;
    size_t count = 0;
    MacroData loaded;

    if (!(in >> magic) || magic != "SGM2")
        return false;

    if (!(in >> loaded.start.percent
              >> loaded.start.tick
              >> loaded.start.x
              >> loaded.start.y))
        return false;

    if (!(in >> loaded.end.percent
              >> loaded.end.tick
              >> loaded.end.x
              >> loaded.end.y))
        return false;

    if (!(in >> count) || count > 1000000)
        return false;

    loaded.events.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        Event e;

        if (!(in >> e.tick >> e.down >> e.button >> e.player1))
            return false;

        loaded.events.push_back(e);
    }

    loaded.valid = !loaded.events.empty();
    g_macro = std::move(loaded);

    return g_macro.valid;
}

void stopPlayback() {
    g_playing = false;
    g_eventIndex = 0;
}

void beginRecord(PlayLayer* layer) {
    if (!layer)
        return;

    g_macro = MacroData{};
    g_macro.start = sampleNow(layer);
    g_recording = true;

    stopPlayback();
    Mod::get()->setSettingValue("auto-playback", false);

    log::info(
        "Segment Macro: recording started at captured {}%",
        g_macro.start.percent
    );
}

void endRecord(PlayLayer* layer) {
    if (!g_recording || !layer)
        return;

    g_macro.end = sampleNow(layer);
    g_recording = false;

    g_macro.valid =
        !g_macro.events.empty() &&
        g_macro.end.tick >= g_macro.start.tick;

    if (g_macro.valid) {
        saveMacro();

        log::info(
            "Segment Macro: saved {} transitions, {}% to {}%",
            g_macro.events.size(),
            g_macro.start.percent,
            g_macro.end.percent
        );
    } else {
        log::warn(
            "Segment Macro: recording contained no button transitions; "
            "previous save was not replaced"
        );
    }
}

bool nearStart(PlayLayer* layer) {
    if (!g_macro.valid || !player(layer))
        return false;

    if (percent(layer) != g_macro.start.percent)
        return false;

    auto p = player(layer);
    constexpr float tolerance = 3.0f;

    return std::fabs(p->getPositionX() - g_macro.start.x) <= tolerance &&
           std::fabs(p->getPositionY() - g_macro.start.y) <= tolerance;
}

bool nearEnd(PlayLayer* layer) {
    if (!g_macro.valid || !player(layer))
        return true;

    auto p = player(layer);

    return percent(layer) >= g_macro.end.percent &&
           std::fabs(p->getPositionX() - g_macro.end.x) <= 8.0f;
}

void tickMacro(PlayLayer* layer) {
    if (!layer)
        return;

    ++g_tick;

    if (!g_recording &&
        !g_playing &&
        settingBool("auto-playback") &&
        g_macro.valid &&
        nearStart(layer)) {

        g_playing = true;
        g_eventIndex = 0;
        g_playStartTick = g_tick;

        log::info(
            "Segment Macro: matched recorded start state; playback takeover"
        );
    }

    if (g_playing) {
        int elapsed = g_tick - g_playStartTick;

        while (g_eventIndex < g_macro.events.size() &&
               g_macro.events[g_eventIndex].tick <= elapsed) {

            auto e = g_macro.events[g_eventIndex++];

            g_forwarding = true;
            layer->handleButton(e.down, e.button, e.player1);
            g_forwarding = false;
        }

        if (nearEnd(layer) ||
            (g_eventIndex >= g_macro.events.size() &&
             percent(layer) >= g_macro.end.percent)) {

            stopPlayback();
            Mod::get()->setSettingValue("auto-playback", false);

            log::info(
                "Segment Macro: endpoint reached; real inputs resumed"
            );
        }
    }
}

CCMenuItemSpriteExtra* makeMenuButton(
    char const* text,
    CCObject* target,
    SEL_MenuHandler selector
) {
    auto label = CCLabelBMFont::create(text, "bigFont.fnt");

    if (!label)
        return nullptr;

    auto button = CCMenuItemSpriteExtra::create(label, target, selector);

    if (button)
        button->setScale(0.42f);

    return button;
}
}

class $modify(SegmentMacroPlayLayer, PlayLayer) {
    bool init(
        GJGameLevel* level,
        bool useReplay,
        bool dontCreateObjects
    ) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        g_tick = 0;
        g_recording = false;
        stopPlayback();

        if (!g_macro.valid)
            loadMacro();

        return true;
    }

    void onQuit() {
        g_recording = false;
        stopPlayback();

        Mod::get()->setSettingValue("auto-playback", false);

        PlayLayer::onQuit();
    }
};

class $modify(SegmentMacroPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();

        if (!menu)
            return;

        menu->setPosition({0.f, 0.f});

        auto win = CCDirector::sharedDirector()->getWinSize();
        float y = win.height * 0.16f;
        float center = win.width * 0.5f;

        auto record = makeMenuButton(
            "RECORD",
            this,
            menu_selector(SegmentMacroPauseLayer::onMacroRecord)
        );

        auto stop = makeMenuButton(
            "STOP / SAVE",
            this,
            menu_selector(SegmentMacroPauseLayer::onMacroStop)
        );

        auto play = makeMenuButton(
            "PLAY SAVED",
            this,
            menu_selector(SegmentMacroPauseLayer::onMacroPlay)
        );

        if (record) {
            menu->addChild(record);
            record->setPosition({center - 105.f, y});
        }

        if (stop) {
            menu->addChild(stop);
            stop->setPosition({center, y});
        }

        if (play) {
            menu->addChild(play);
            play->setPosition({center + 105.f, y});
        }

        this->addChild(menu, 100);
    }

    void onMacroRecord(CCObject*) {
        auto layer = PlayLayer::get();

        if (!layer)
            return;

        if (!layer->m_isPracticeMode) {
            FLAlertLayer::create(
                "Segment Macro",
                "Enter Practice Mode before recording a segment.",
                "OK"
            )->show();

            return;
        }

        if (g_recording) {
            FLAlertLayer::create(
                "Segment Macro",
                "Recording is already active. Use STOP / SAVE to finish it.",
                "OK"
            )->show();

            return;
        }

        beginRecord(layer);

        FLAlertLayer::create(
            "Segment Macro",
            "Recording started. Resume gameplay, then pause and choose "
            "STOP / SAVE.",
            "OK"
        )->show();
    }

    void onMacroStop(CCObject*) {
        auto layer = PlayLayer::get();

        if (!layer)
            return;

        if (g_recording) {
            endRecord(layer);

            FLAlertLayer::create(
                "Segment Macro",
                g_macro.valid
                    ? "Segment saved."
                    : "No valid segment was saved. Record at least one "
                      "button transition.",
                "OK"
            )->show();

            return;
        }

        if (g_playing) {
            stopPlayback();
            Mod::get()->setSettingValue("auto-playback", false);

            FLAlertLayer::create(
                "Segment Macro",
                "Playback stopped.",
                "OK"
            )->show();

            return;
        }

        FLAlertLayer::create(
            "Segment Macro",
            "Nothing is currently recording or playing.",
            "OK"
        )->show();
    }

    void onMacroPlay(CCObject*) {
        if (!g_macro.valid && !loadMacro()) {
            FLAlertLayer::create(
                "Segment Macro",
                "No saved macro was found. Record a segment first.",
                "OK"
            )->show();

            return;
        }

        Mod::get()->setSettingValue("auto-playback", true);

        FLAlertLayer::create(
            "Segment Macro",
            "Playback armed. Resume and reach the recorded start position "
            "to trigger playback.",
            "OK"
        )->show();
    }
};

class $modify(SegmentMacroBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto layer = PlayLayer::get();

        if (!g_forwarding && layer && button == 1 && isPlayer1) {
            if (g_recording) {
                g_macro.events.push_back({
                    g_tick - g_macro.start.tick,
                    down,
                    button,
                    isPlayer1
                });
            }

            if (g_playing)
                return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(SegmentMacroPlayerObject, PlayerObject) {
    void update(float dt) {
        auto layer = PlayLayer::get();

        if (layer && this == layer->m_player1)
            tickMacro(layer);

        PlayerObject::update(dt);
    }
};
