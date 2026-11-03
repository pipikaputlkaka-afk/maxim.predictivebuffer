#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace geode::prelude;

using Clock = std::chrono::steady_clock;

// F6 cycles the selected gamemode's max shift through these values (in physics ticks).
static constexpr int PRESETS[] = {1, 2, 3, 5, 10, 20, 30, 60, 90, 120};
static constexpr int MAX_SHIFT = 120;
// How many ticks ahead the simulation looks to decide if a click is safe.
static constexpr int HORIZON = 40;
static constexpr int JUMP_BUTTON = 1;

// F7 cycles through these gamemodes.
static char const* MODES[] = {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"};
static constexpr int MODE_COUNT = 8;
static int g_selected = 0;

static void say(std::string const& msg) {
    log::info("{}", msg);
    std::printf("[PredictiveBuffer] %s\n", msg.c_str());
    std::fflush(stdout);
}

static bool isEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

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
    say("Selected: " + mode + " | max shift: " + std::to_string(windowFor(mode)) + " ticks | " + (modeOn(mode) ? "ON" : "OFF"));
}

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
    say("Loaded (lookahead build 1). F7 = choose gamemode, F6 = change its max shift.");
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
    say(mode + " max shift -> " + std::to_string(next) + " ticks");
}

static void selectNextMode() {
    g_selected = (g_selected + 1) % MODE_COUNT;
    printStatus(MODES[g_selected]);
}

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

// ---------------------------------------------------------------------------
// Predictive click correction
// ---------------------------------------------------------------------------

struct Ev {
    bool down;
    bool p1;
    int ticks;
};

static std::vector<Ev> g_incoming;  // clicks held back, waiting to be judged
static std::vector<Ev> g_pending;   // clicks that were moved to a later tick
static bool g_sim = false;          // true while simulating ahead
static bool g_simDead = false;      // set if the simulated player would die
static bool g_pass = false;         // true while we send a click to the game ourselves
static float g_lastDt = 1.f / 240.f;
static bool g_lastHalf = false;
static bool g_lastLast = true;

static bool inPlay(GJBaseGameLayer* l) {
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == l;
}

// Sends a click to the game right now, bypassing our own hook.
static void applyNow(GJBaseGameLayer* l, Ev const& e) {
    g_pass = true;
    l->handleButton(e.down, JUMP_BUTTON, e.p1);
    g_pass = false;
}

// Plays the future with the click happening after `d` ticks.
// Returns true if the player survives HORIZON ticks after the click.
static bool simulate(GJBaseGameLayer* layer, Ev const& e, int d, Clock::time_point deadline, bool& timedOut) {
    g_sim = true;
    g_simDead = false;
    bool alive = true;
    int total = d + HORIZON;
    for (int t = 0; t < total; t++) {
        for (auto const& q : g_pending) {
            if (q.ticks == t) applyNow(layer, q);
        }
        if (t == d) applyNow(layer, e);

        layer->processCommands(g_lastDt, g_lastHalf, g_lastLast);

        if (g_simDead || (layer->m_player1 && layer->m_player1->m_isDead)) {
            alive = false;
            break;
        }
        if (Clock::now() > deadline) {
            timedOut = true;
            alive = false;
            break;
        }
    }
    g_sim = false;
    return alive;
}

static void decide(GJBaseGameLayer* layer, Ev e) {
    auto pl = PlayLayer::get();
    PlayerObject* p = e.p1 ? layer->m_player1 : layer->m_player2;
    if (!pl || !p || p->m_isDead) {
        applyNow(layer, e);
        return;
    }

    std::string mode = modeName(p);
    int W = (std::min)(windowFor(mode), MAX_SHIFT);
    std::string what = std::string("Click ") + (e.down ? "DOWN" : "UP") + " (" + mode + "): ";

    // Keep clicks in order if an earlier one for this player is still waiting.
    int minTicks = 0;
    for (auto const& q : g_pending) {
        if (q.p1 == e.p1) minTicks = (std::max)(minTicks, q.ticks);
    }

    int best = -1;
    bool timedOut = false;
    double ms = 0.0;
    if (W > 0) {
        CheckpointObject* cp = pl->createCheckpoint();
        if (cp) {
            Ref<CheckpointObject> keep(cp);
            auto t0 = Clock::now();
            auto deadline = t0 + std::chrono::milliseconds(20);
            for (int d = 0; d <= W; d++) {
                bool alive = simulate(layer, e, d, deadline, timedOut);
                pl->loadFromCheckpoint(cp);
                if (timedOut) break;
                if (alive) {
                    best = d;
                    break;
                }
            }
            ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        }
    }

    int finalTicks = (best >= 0) ? (std::max)(best, minTicks) : minTicks;

    if (timedOut) {
        say(what + "lookahead too slow (>20 ms), sent unchanged. Lower the max shift with F6.");
    } else if (best > 0) {
        say(what + "would die -> moved " + std::to_string(best) + " ticks later (" + std::to_string(ms) + " ms)");
    } else if (best == 0) {
        say(what + "safe as-is (" + std::to_string(ms) + " ms)");
    } else {
        say(what + "no safe moment within " + std::to_string(W) + " ticks, sent unchanged");
    }

    if (finalTicks <= 0) {
        applyNow(layer, e);
    } else {
        g_pending.push_back({e.down, e.p1, finalTicks});
    }
}

class $modify(PBScheduler, CCScheduler) {
    void update(float dt) {
        pollKeys();
        CCScheduler::update(dt);
    }
};

class $modify(PBPlay, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_sim) {
            g_simDead = true;  // simulated death: don't actually kill the player
            return;
        }
        g_incoming.clear();
        g_pending.clear();
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        g_incoming.clear();
        g_pending.clear();
        PlayLayer::resetLevel();
    }
};

class $modify(PBLayer, GJBaseGameLayer) {
    // Every click in the game goes through here. We hold it back and judge it
    // after the current physics tick has finished.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (button != JUMP_BUTTON || g_pass || g_sim || !isEnabled() || !inPlay(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        if (!p || p->m_isDead || !modeOn(modeName(p))) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        g_incoming.push_back({down, isPlayer1, 0});
    }

    // Runs every physics tick.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (g_sim) return;

        g_lastDt = dt;
        g_lastHalf = isHalfTick;
        g_lastLast = isLastTick;

        if (!inPlay(this)) {
            // Not in a normal level: never hold clicks back.
            for (auto const& e : g_incoming) applyNow(this, e);
            g_incoming.clear();
            g_pending.clear();
            return;
        }

        // 1. Send out clicks that were moved to this tick.
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (--it->ticks <= 0) {
                applyNow(this, *it);
                say("  moved click applied");
                it = g_pending.erase(it);
            } else {
                ++it;
            }
        }

        // 2. Judge the clicks that just arrived.
        if (!g_incoming.empty()) {
            auto events = std::move(g_incoming);
            g_incoming.clear();
            for (auto const& e : events) decide(this, e);
        }
    }
};
