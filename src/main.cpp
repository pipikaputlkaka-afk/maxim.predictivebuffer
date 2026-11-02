#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

#include <cstdio>
#include <string>

using namespace geode::prelude;

// F6 cycles the selected gamemode through these buffer windows (in physics ticks).
static constexpr int PRESETS[] = {1, 5, 10, 20, 30, 60, 90, 120, 240, 480};

// F7 cycles through these gamemodes.
static char const* MODES[] = {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"};
static constexpr int MODE_COUNT = 8;
static int g_selected = 0;

static constexpr int JUMP_BUTTON = 1;

static void say(std::string const& msg) {
    log::info("{}", msg);
    std::printf("[PredictiveBuffer] %s\n", msg.c_str());
    std::fflush(stdout);
}

static bool isEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

// Default window for gamemodes that have no value saved yet.
static int defaultWindow() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("tick-window"));
}

static int windowFor(std::string const& mode) {
    return static_cast<int>(
        Mod::get()->getSavedValue<int64_t>("window-" + mode, static_cast<int64_t>(defaultWindow()))
    );
}

static bool modeOn(std::string const& mode) {
    return Mod::get()->getSettingValue<bool>("buffer-" + mode);
}

static void printStatus(std::string const& mode) {
    say("Selected: " + mode + " | window: " + std::to_string(windowFor(mode)) + " ticks | " + (modeOn(mode) ? "ON" : "OFF"));
}

// Opens the separate console window when GD starts.
$on_mod(Loaded) {
#ifdef GEODE_IS_WINDOWS
    if (AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
        SetConsoleTitleA("Predictive Buffer");
    }
#endif
    say("Loaded. F7 = choose gamemode, F6 = change its tick window.");
    say(std::string("Master switch: ") + (isEnabled() ? "ON" : "OFF"));
    for (int i = 0; i < MODE_COUNT; i++) {
        std::string m = MODES[i];
        say("  " + m + ": " + std::to_string(windowFor(m)) + " ticks, " + (modeOn(m) ? "ON" : "OFF"));
    }
    printStatus(MODES[g_selected]);
}

static void cycleWindow() {
    std::string mode = MODES[g_selected];
    int cur = windowFor(mode);
    int next = PRESETS[0];
    for (int p : PRESETS) {
        if (p > cur) {
            next = p;
            break;
        }
    }
    Mod::get()->setSavedValue<int64_t>("window-" + mode, static_cast<int64_t>(next));
    say(mode + " window -> " + std::to_string(next) + " ticks");
}

static void selectNextMode() {
    g_selected = (g_selected + 1) % MODE_COUNT;
    printStatus(MODES[g_selected]);
}

// Checks the keys every frame, only while the game window is focused.
static void pollKeys() {
#ifdef GEODE_IS_WINDOWS
    static bool w6 = false, w7 = false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool focused = (pid == GetCurrentProcessId());

    auto edge = [&](int vk, bool& was) {
        bool down = focused && ((GetAsyncKeyState(vk) & 0x8000) != 0);
        bool pressed = down && !was;
        was = down;
        return pressed;
    };

    if (edge(VK_F6, w6)) cycleWindow();
    if (edge(VK_F7, w7)) selectNextMode();
#endif
}

static char const* modeName(PlayerObject* p) {
    if (p->m_isShip) return "ship";
    if (p->m_isBird) return "ufo";
    if (p->m_isBall) return "ball";
    if (p->m_isDart) return "wave";
    if (p->m_isRobot) return "robot";
    if (p->m_isSpider) return "spider";
    if (p->m_isSwing) return "swing";
    return "cube";
}

static bool touchingRing(PlayerObject* p) {
    return p->m_touchingRings && p->m_touchingRings->count() > 0;
}

// Modes that jump off the ground.
static bool groundMode(PlayerObject* p) {
    return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
}

// Returns why a click would take effect right now, or nullptr if it wouldn't.
static char const* actReason(PlayerObject* p) {
    if (touchingRing(p)) return "orb";
    if (groundMode(p) && p->m_isOnGround) return "landing";
    return nullptr;
}

class $modify(PBScheduler, CCScheduler) {
    void update(float dt) {
        pollKeys();
        CCScheduler::update(dt);
    }
};

class $modify(PBLayer, GJBaseGameLayer) {
    struct Fields {
        int ticksLeft[2] = {0, 0};
        bool held[2] = {false, false};
        bool firing = false;
    };

    // Every click in the game goes through here.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto f = m_fields.self();
        if (button == JUMP_BUTTON && !f->firing) {
            int i = isPlayer1 ? 0 : 1;
            PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
            if (!down) {
                f->held[i] = false;
            } else if (p) {
                std::string mode = modeName(p);
                if (isEnabled() && modeOn(mode)) {
                    char const* reason = actReason(p);
                    f->held[i] = true;
                    say("Jump press (" + mode + "): ground=" + (p->m_isOnGround ? "yes" : "no") +
                        ", orb=" + (touchingRing(p) ? "yes" : "no") +
                        " -> " + (reason ? "acts now" : "BUFFERED " + std::to_string(windowFor(mode)) + " ticks"));
                    f->ticksLeft[i] = reason ? 0 : windowFor(mode);
                } else {
                    f->ticksLeft[i] = 0;
                }
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Runs every physics tick.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        auto f = m_fields.self();
        for (int i = 0; i < 2; i++) {
            if (f->ticksLeft[i] <= 0) continue;
            PlayerObject* p = (i == 0) ? m_player1 : m_player2;
            if (!p) {
                f->ticksLeft[i] = 0;
                continue;
            }
            std::string mode = modeName(p);
            if (!isEnabled() || !modeOn(mode)) {
                f->ticksLeft[i] = 0;
                continue;
            }

            f->ticksLeft[i]--;

            char const* reason = actReason(p);
            if (reason) {
                f->ticksLeft[i] = 0;
                f->firing = true;
                bool isP1 = (i == 0);
                GJBaseGameLayer::handleButton(false, JUMP_BUTTON, isP1);
                GJBaseGameLayer::handleButton(true, JUMP_BUTTON, isP1);
                if (!f->held[i]) {
                    GJBaseGameLayer::handleButton(false, JUMP_BUTTON, isP1);
                }
                f->firing = false;
                say("Buffered click FIRED (" + mode + ", " + reason + ")");
            }
        }
    }
};
