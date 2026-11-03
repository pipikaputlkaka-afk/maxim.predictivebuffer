#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace geode::prelude;

using Clock = std::chrono::steady_clock;

// F6 cycles the selected gamemode's max shift through these values (in physics ticks).
static constexpr int PRESETS[] = {1, 2, 3, 5, 10, 20, 30, 60, 90, 120};
static constexpr int MAX_SHIFT = 120;
// How many ticks ahead the prediction looks after a click.
static constexpr int HORIZON = 40;
static constexpr int JUMP_BUTTON = 1;
// Half the size of the wave's hitbox, used for the prediction.
static constexpr float HALF = 5.f;

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
    say("Selected: " + mode + " | max shift: " + std::to_string(windowFor(mode)) + " ticks | " + (modeOn(mode) ? "ON" : "OFF") +
        (mode == "wave" ? "" : " (note: only wave is corrected)"));
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
    say("Loaded (math build 1, wave only). F7 = choose gamemode, F6 = change its max shift.");
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

// ---------------------------------------------------------------------------
// Wave click prediction (pure math, no game state is touched)
// ---------------------------------------------------------------------------

struct Ev {
    bool down;
    bool p1;
    int ticks;
};

struct HRect {
    float x0, y0, x1, y1;
};

static std::vector<Ev> g_incoming;  // clicks held back, waiting to be judged
static std::vector<Ev> g_pending;   // clicks that were moved to a later tick
static bool g_pass = false;         // true while we send a click to the game ourselves

// Measured per player (index 0 = player 1, 1 = player 2), per physics tick.
static bool g_hasPrev[2] = {false, false};
static float g_prevX[2] = {0.f, 0.f};
static float g_prevY[2] = {0.f, 0.f};
static float g_dx[2] = {0.f, 0.f};   // horizontal movement per tick
static float g_vy[2] = {0.f, 0.f};   // vertical movement per tick (last tick)
static float g_mag[2] = {0.f, 0.f};  // wave's vertical speed per tick

// Cached hazard hitboxes of the current level, sorted by left edge.
static std::vector<HRect> g_haz;
static void* g_hazOwner = nullptr;
static int g_hazCount = -1;

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

// Measures how far each player moves per physics tick.
static void track(GJBaseGameLayer* l) {
    for (int i = 0; i < 2; i++) {
        PlayerObject* p = (i == 0) ? l->m_player1 : l->m_player2;
        if (!p) {
            g_hasPrev[i] = false;
            continue;
        }
        auto pos = p->getPosition();
        if (g_hasPrev[i]) {
            float dx = pos.x - g_prevX[i];
            float dy = pos.y - g_prevY[i];
            if (dx > 0.001f && dx < 40.f && std::fabs(dy) < 60.f) {
                g_dx[i] = dx;
                g_vy[i] = dy;
                if (p->m_isDart && std::fabs(dy) > 0.001f) g_mag[i] = std::fabs(dy);
            }
        }
        if (!p->m_isDart) g_mag[i] = 0.f;
        g_prevX[i] = pos.x;
        g_prevY[i] = pos.y;
        g_hasPrev[i] = true;
    }
}

static void ensureHazards(GJBaseGameLayer* l) {
    int count = l->m_objects ? static_cast<int>(l->m_objects->count()) : 0;
    if (g_hazOwner == l && g_hazCount == count) return;

    auto t0 = Clock::now();
    g_haz.clear();
    if (l->m_objects) {
        for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
            if (!obj) continue;
            if (obj->m_objectType != GameObjectType::Hazard) continue;
            auto r = obj->getObjectRect();
            g_haz.push_back({r.origin.x, r.origin.y, r.origin.x + r.size.width, r.origin.y + r.size.height});
        }
    }
    std::sort(g_haz.begin(), g_haz.end(), [](HRect const& a, HRect const& b) { return a.x0 < b.x0; });
    g_hazOwner = l;
    g_hazCount = count;
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    say("Hazard list built: " + std::to_string(g_haz.size()) + " hazards (" + std::to_string(ms) + " ms)");
}

static void decide(GJBaseGameLayer* layer, Ev e) {
    int i = e.p1 ? 0 : 1;
    PlayerObject* p = e.p1 ? layer->m_player1 : layer->m_player2;
    if (!p || p->m_isDead || !p->m_isDart) {
        applyNow(layer, e);
        return;
    }

    int W = (std::min)(windowFor("wave"), MAX_SHIFT);
    std::string what = std::string("Click ") + (e.down ? "DOWN" : "UP") + " (wave): ";
    float dx = g_dx[i];
    if (W <= 0 || dx <= 0.f) {
        applyNow(layer, e);
        say(what + "no speed measured yet, sent unchanged");
        return;
    }
    float mag = g_mag[i] > 0.f ? g_mag[i] : dx;

    auto t0 = Clock::now();
    ensureHazards(layer);

    auto pos = p->getPosition();
    bool upside = p->m_isUpsideDown;

    // Collect only the hazards the wave could reach.
    float span = static_cast<float>(W + HORIZON + 2);
    float xmin = pos.x - 20.f;
    float xmax = pos.x + span * dx + 20.f;
    float reach = span * mag + 20.f;
    std::vector<HRect> nearH;
    {
        auto it = std::lower_bound(g_haz.begin(), g_haz.end(), xmin - 150.f,
            [](HRect const& h, float v) { return h.x0 < v; });
        for (; it != g_haz.end() && it->x0 <= xmax; ++it) {
            if (it->x1 < xmin) continue;
            if (it->y1 < pos.y - reach || it->y0 > pos.y + reach) continue;
            nearH.push_back(*it);
        }
    }

    auto vyFor = [&](bool down) {
        float v = down ? mag : -mag;
        return upside ? -v : v;
    };

    // Would the wave survive if this click happens after `d` ticks?
    auto survives = [&](int d) {
        float x = pos.x, y = pos.y, vy = g_vy[i];
        int total = d + HORIZON;
        for (int t = 0; t < total; t++) {
            for (auto const& q : g_pending) {
                if (q.p1 == e.p1 && q.ticks == t) vy = vyFor(q.down);
            }
            if (t == d) vy = vyFor(e.down);
            x += dx;
            y += vy;
            for (auto const& h : nearH) {
                if (x + HALF > h.x0 && x - HALF < h.x1 && y + HALF > h.y0 && y - HALF < h.y1) return false;
            }
        }
        return true;
    };

    // Keep clicks in order if an earlier one for this player is still waiting.
    int minTicks = 0;
    for (auto const& q : g_pending) {
        if (q.p1 == e.p1) minTicks = (std::max)(minTicks, q.ticks);
    }

    int best = -1;
    for (int d = 0; d <= W; d++) {
        if (survives(d)) {
            best = d;
            break;
        }
    }
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::string info = " [speed " + std::to_string(dx) + "/" + std::to_string(mag) + ", " +
                       std::to_string(nearH.size()) + " hazards near, " + std::to_string(ms) + " ms]";

    int finalTicks = (best >= 0) ? (std::max)(best, minTicks) : minTicks;

    if (best > 0) {
        say(what + "would die -> moved " + std::to_string(best) + " ticks later" + info);
    } else if (best == 0) {
        say(what + "safe as-is" + info);
    } else {
        say(what + "no safe moment within " + std::to_string(W) + " ticks, sent unchanged" + info);
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
        g_incoming.clear();
        g_pending.clear();
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        g_incoming.clear();
        g_pending.clear();
        g_hasPrev[0] = g_hasPrev[1] = false;
        PlayLayer::resetLevel();
    }
};

class $modify(PBLayer, GJBaseGameLayer) {
    // Every click in the game goes through here. Wave clicks are held back and
    // judged after the current physics tick. Everything else passes straight through.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (button != JUMP_BUTTON || g_pass || !isEnabled() || !inPlay(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        if (!p || p->m_isDead || !p->m_isDart || !modeOn("wave")) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        g_incoming.push_back({down, isPlayer1, 0});
    }

    // Runs every physics tick.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (!inPlay(this)) {
            for (auto const& e : g_incoming) applyNow(this, e);
            g_incoming.clear();
            g_pending.clear();
            return;
        }

        track(this);

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
