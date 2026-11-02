#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCScheduler.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

#include <cstdio>
#include <string>

using namespace geode::prelude;

// F6 cycles through these buffer windows (in physics ticks).
// Every value must be <= the "max" of tick-window in mod.json.
static constexpr int PRESETS[] = {1, 5, 10, 20, 30, 60, 90, 120, 240, 480};

static void say(std::string const& msg) {
    log::info("{}", msg);
    std::printf("[PredictiveBuffer] %s\n", msg.c_str());
    std::fflush(stdout);
}

static bool isEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

static int getWindow() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("tick-window"));
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
    say("Loaded. Press F6 in-game to cycle the buffer window.");
    say("Current window: " + std::to_string(getWindow()) + " ticks, enabled: " + (isEnabled() ? "yes" : "no"));
}

static void cycleWindow() {
    int cur = getWindow();
    int next = PRESETS[0];
    for (int p : PRESETS) {
        if (p > cur) {
            next = p;
            break;
        }
    }
    Mod::get()->setSettingValue<int64_t>("tick-window", next);
    say("Buffer window -> " + std::to_string(next) + " ticks");
}

// Checks F6 every frame, only while the game window is focused.
static void pollF6() {
#ifdef GEODE_IS_WINDOWS
    static bool wasDown = false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool focused = (pid == GetCurrentProcessId());
    bool down = focused && ((GetAsyncKeyState(VK_F6) & 0x8000) != 0);
    if (down && !wasDown) {
        cycleWindow();
    }
    wasDown = down;
#endif
}

// Gamemode on/off switches live in the mod settings (buffer-*).
static bool gamemodeOn(PlayerObject* p) {
    char const* key = "buffer-cube";
    if (p->m_isShip) key = "buffer-ship";
    else if (p->m_isBird) key = "buffer-ufo";
    else if (p->m_isBall) key = "buffer-ball";
    else if (p->m_isDart) key = "buffer-wave";
    else if (p->m_isRobot) key = "buffer-robot";
    else if (p->m_isSpider) key = "buffer-spider";
    else if (p->m_isSwing) key = "buffer-swing";
    return Mod::get()->getSettingValue<bool>(key);
}

class $modify(PBScheduler, CCScheduler) {
    void update(float dt) {
        pollF6();
        CCScheduler::update(dt);
    }
};

class $modify(PBPlayer, PlayerObject) {
    struct Fields {
        int ticksLeft = 0;
        bool held = false;
        bool firing = false;
    };

    bool pushButton(PlayerButton btn) {
        bool res = PlayerObject::pushButton(btn);
        auto f = m_fields.self();
        if (btn == PlayerButton::Jump && !f->firing && isEnabled() && gamemodeOn(this)) {
            f->ticksLeft = getWindow();
            f->held = true;
        }
        return res;
    }

    bool releaseButton(PlayerButton btn) {
        if (btn == PlayerButton::Jump) {
            m_fields->held = false;
        }
        return PlayerObject::releaseButton(btn);
    }

    // Runs once per physics tick.
    void update(float dt) {
        PlayerObject::update(dt);

        auto f = m_fields.self();
        if (f->ticksLeft <= 0) return;
        if (!isEnabled() || !gamemodeOn(this)) {
            f->ticksLeft = 0;
            return;
        }

        f->ticksLeft--;

        // Player is touching an orb: fire the buffered click now.
        if (m_touchingRings && m_touchingRings->count() > 0) {
            f->firing = true;
            f->ticksLeft = 0;
            PlayerObject::releaseButton(PlayerButton::Jump);
            PlayerObject::pushButton(PlayerButton::Jump);
            if (!f->held) {
                PlayerObject::releaseButton(PlayerButton::Jump);
            }
            f->firing = false;
            say("Buffered click fired on orb");
        }
    }
};
