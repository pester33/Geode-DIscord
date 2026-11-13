#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

// Built-in Discord Application ID (the "Geometry Dash" app shown on profiles).
// Players can override it in the mod settings.
static constexpr auto DEFAULT_CLIENT_ID = "1570850924069658797";

// GD Gaming List data (backup copy kept up to date by the List-Mod repo)
static constexpr auto GDGL_DATA_URL = "https://raw.githubusercontent.com/pester33/List-Mod/main/data/levels.json";
static constexpr auto GDGL_LOGO_URL = "https://raw.githubusercontent.com/pester33/List-Mod/main/resources/gdgl_logo.png";

static int64_t unixNow() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

static std::string clientID() {
    auto id = utils::string::trim(Mod::get()->getSettingValue<std::string>("client-id"));
    return id.empty() ? std::string(DEFAULT_CLIENT_ID) : id;
}

// ---------------------------------------------------------------------------
// Discord IPC (talks to the Discord desktop app through its local pipe)
// Runs on its own thread so GD never freezes waiting for Discord.
// ---------------------------------------------------------------------------

class DiscordPresence {
public:
    static DiscordPresence& get() {
        static DiscordPresence instance;
        return instance;
    }

    // Queue an activity (or clear it with nullopt). Only the newest one is sent.
    void set(std::optional<matjson::Value> activity) {
        {
            std::lock_guard lock(m_mutex);
            m_pending = std::move(activity);
            m_hasPending = true;
        }
        this->ensureThread();
        m_cv.notify_one();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<matjson::Value> m_pending;
    bool m_hasPending = false;
    std::atomic<bool> m_started = false;

    void ensureThread() {
        if (m_started.exchange(true)) return;
        std::thread([this] { this->run(); }).detach();
    }

#ifdef GEODE_IS_WINDOWS
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::string m_connectedID;
    int m_nonce = 0;

    void disconnect() {
        if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        m_connectedID.clear();
    }

    bool writeFrame(uint32_t opcode, std::string const& json) {
        std::string buf(8 + json.size(), '\0');
        uint32_t len = static_cast<uint32_t>(json.size());
        std::memcpy(buf.data(), &opcode, 4);
        std::memcpy(buf.data() + 4, &len, 4);
        std::memcpy(buf.data() + 8, json.data(), json.size());
        DWORD written = 0;
        return WriteFile(m_pipe, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) && written == buf.size();
    }

    // Reads one reply frame, waiting up to `timeoutMs` for it. Returns false on error/timeout.
    bool readFrame(std::string& out, int timeoutMs = 3000) {
        auto waitFor = [&](DWORD need) {
            for (int waited = 0; waited < timeoutMs; waited += 20) {
                DWORD avail = 0;
                if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
                if (avail >= need) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };
        if (!waitFor(8)) return false;
        uint32_t header[2] = { 0, 0 };
        DWORD read = 0;
        if (!ReadFile(m_pipe, header, 8, &read, nullptr) || read != 8) return false;
        out.assign(header[1], '\0');
        DWORD got = 0;
        while (got < header[1]) {
            if (!waitFor(1)) return false;
            DWORD chunk = 0;
            if (!ReadFile(m_pipe, out.data() + got, header[1] - got, &chunk, nullptr)) return false;
            got += chunk;
        }
        return header[0] != 2; // opcode 2 = Discord closed the connection
    }

    bool connect(std::string const& id) {
        for (int i = 0; i < 10; i++) {
            auto name = fmt::format("\\\\.\\pipe\\discord-ipc-{}", i);
            HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) continue;
            m_pipe = pipe;
            auto hello = matjson::makeObject({ { "v", 1 }, { "client_id", id } }).dump(matjson::NO_INDENTATION);
            std::string reply;
            if (this->writeFrame(0, hello) && this->readFrame(reply) && reply.find("\"READY\"") != std::string::npos) {
                m_connectedID = id;
                log::info("[discord] Connected to Discord (pipe {})", i);
                return true;
            }
            log::warn("[discord] Handshake failed on pipe {}: {}", i, reply.substr(0, 200));
            this->disconnect();
        }
        return false;
    }

    bool send(std::optional<matjson::Value> const& activity) {
        auto args = matjson::makeObject({ { "pid", static_cast<int>(GetCurrentProcessId()) } });
        if (activity) args["activity"] = *activity;
        auto frame = matjson::makeObject({
            { "cmd", "SET_ACTIVITY" },
            { "args", args },
            { "nonce", std::to_string(++m_nonce) },
        });
        if (!this->writeFrame(1, frame.dump(matjson::NO_INDENTATION))) return false;
        std::string reply;
        if (!this->readFrame(reply)) return false;
        if (reply.find("\"ERROR\"") != std::string::npos) log::warn("[discord] Discord said: {}", reply.substr(0, 300));
        return true;
    }

    void run() {
        using clock = std::chrono::steady_clock;
        auto lastSend = clock::now() - std::chrono::seconds(10);
        while (true) {
            std::optional<matjson::Value> activity;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return m_hasPending; });
            }
            // Discord allows about 5 updates per 20 seconds; keep at least 4s between them
            auto since = clock::now() - lastSend;
            if (since < std::chrono::seconds(4)) std::this_thread::sleep_for(std::chrono::seconds(4) - since);
            {
                std::lock_guard lock(m_mutex);
                activity = m_pending;
                m_hasPending = false;
            }

            auto id = clientID();
            if (id.empty()) {
                log::warn("[discord] No Discord Application ID set; nothing will show on Discord");
                continue;
            }
            if (m_pipe != INVALID_HANDLE_VALUE && m_connectedID != id) this->disconnect();
            if (m_pipe == INVALID_HANDLE_VALUE && !this->connect(id)) {
                // Discord isn't open (or not reachable): try again in 15s with the newest activity
                std::this_thread::sleep_for(std::chrono::seconds(15));
                std::lock_guard lock(m_mutex);
                if (!m_hasPending) { m_pending = activity; m_hasPending = true; }
                continue;
            }
            if (!this->send(activity)) {
                log::warn("[discord] Lost connection to Discord, will reconnect");
                this->disconnect();
                std::lock_guard lock(m_mutex);
                if (!m_hasPending) { m_pending = activity; m_hasPending = true; }
            }
            lastSend = clock::now();
        }
    }
#else
    void run() {
        log::info("[discord] Discord Rich Presence only works on Windows");
    }
#endif
};

// ---------------------------------------------------------------------------
// GD Gaming List ranks (level ID -> "#12" / "Extended #80")
// ---------------------------------------------------------------------------

struct GdglEntry {
    int position = 0;
    std::string bucket;
};
static std::unordered_map<std::string, GdglEntry> s_gdgl;
static async::TaskHolder<web::WebResponse> s_gdglTask;
static bool s_gdglLoaded = false;
static void refreshPresence();

static void loadGdgl() {
    if (s_gdglLoaded || !Mod::get()->getSettingValue<bool>("show-gdgl")) return;
    s_gdglLoaded = true;
    web::WebRequest req;
    req.timeout(std::chrono::seconds(20));
    s_gdglTask.spawn(req.get(GDGL_DATA_URL), [](web::WebResponse res) {
        if (!res.ok()) {
            log::warn("[discord] Couldn't load GDGL list: HTTP {}", res.code());
            s_gdglLoaded = false;
            return;
        }
        auto json = res.json().unwrapOr(matjson::Value());
        if (!json.isArray()) return;
        for (auto& e : json) {
            std::string id;
            if (e["levelId"].isString()) id = e["levelId"].asString().unwrapOr("");
            else if (e["levelId"].isNumber()) id = std::to_string(e["levelId"].asInt().unwrapOr(0));
            if (id.empty() || id == "0") continue;
            s_gdgl[id] = { static_cast<int>(e["position"].asInt().unwrapOr(0)), e["bucket"].asString().unwrapOr("main") };
        }
        log::info("[discord] Loaded {} GDGL levels", s_gdgl.size());
        refreshPresence();
    });
}

static std::optional<GdglEntry> gdglFor(GJGameLevel* level) {
    if (!level || !Mod::get()->getSettingValue<bool>("show-gdgl")) return std::nullopt;
    auto it = s_gdgl.find(std::to_string(level->m_levelID.value()));
    if (it == s_gdgl.end()) return std::nullopt;
    return it->second;
}

// ---------------------------------------------------------------------------
// What GD is doing right now -> Discord activity
// ---------------------------------------------------------------------------

enum class Screen { Menu, Browsing, Viewing, Playing, Editing, EditorMenu };

struct GameState {
    Screen screen = Screen::Menu;
    Ref<GJGameLevel> level = nullptr;
    int attempts = 0;
    bool practice = false;
    bool completed = false;
    int64_t since = unixNow();
};
static GameState s_state;
static int64_t s_gameStart = unixNow();

static std::string levelTitle(GJGameLevel* level) {
    if (!level) return "a level";
    std::string name = level->m_levelName;
    std::string creator = level->m_creatorName;
    if (name.empty()) name = "Unnamed level";
    if (creator.empty() || level->m_levelID.value() <= 0) return name;
    return fmt::format("{} by {}", name, creator);
}

static void refreshPresence() {
    auto mod = Mod::get();
    if (!mod->getSettingValue<bool>("enabled")) {
        DiscordPresence::get().set(std::nullopt);
        return;
    }
    bool showNames = mod->getSettingValue<bool>("show-level-name");
    bool showProgress = mod->getSettingValue<bool>("show-progress");
    auto& st = s_state;
    auto gdgl = gdglFor(st.level);

    std::string details, state;
    int64_t start = st.since;
    switch (st.screen) {
        case Screen::Menu:
            details = "In menus";
            start = s_gameStart;
            break;
        case Screen::Browsing:
            details = "Browsing levels";
            start = s_gameStart;
            break;
        case Screen::Viewing:
            details = "Looking at a level";
            if (showNames) state = levelTitle(st.level);
            start = s_gameStart;
            break;
        case Screen::EditorMenu:
            details = "In the editor";
            if (showNames && st.level) state = std::string(st.level->m_levelName);
            break;
        case Screen::Editing:
            details = "Editing a level";
            if (showNames && st.level) state = std::string(st.level->m_levelName);
            break;
        case Screen::Playing: {
            details = showNames ? levelTitle(st.level) : "Playing a level";
            std::vector<std::string> parts;
            if (gdgl) parts.push_back(gdgl->bucket == "main" ? fmt::format("GDGL #{}", gdgl->position)
                                                             : fmt::format("GDGL Extended #{}", gdgl->position));
            if (st.completed) parts.push_back("Beat it!");
            else if (st.practice) parts.push_back("Practice mode");
            if (showProgress && st.level) {
                if (!st.level->isPlatformer()) parts.push_back(fmt::format("Best {}%", st.level->m_normalPercent.value()));
                parts.push_back(fmt::format("Attempt {}", st.attempts));
            }
            for (size_t i = 0; i < parts.size(); i++) state += (i ? " | " : "") + parts[i];
            break;
        }
    }

    auto activity = matjson::makeObject({
        { "details", details.substr(0, 127) },
        { "timestamps", matjson::makeObject({ { "start", start } }) },
    });
    if (!state.empty()) activity["state"] = state.substr(0, 127);
    if (state.size() == 1) activity["state"] = state + " "; // Discord needs at least 2 characters

    auto assets = matjson::makeObject({ { "large_image", "logo" }, { "large_text", "Geometry Dash" } });
    if (gdgl && st.screen == Screen::Playing) {
        assets["small_image"] = GDGL_LOGO_URL;
        assets["small_text"] = "GD Gaming List";
    }
    activity["assets"] = assets;

    DiscordPresence::get().set(activity);
}

static void setScreen(Screen screen, GJGameLevel* level = nullptr) {
    s_state.screen = screen;
    s_state.level = level;
    s_state.attempts = 0;
    s_state.practice = false;
    s_state.completed = false;
    s_state.since = unixNow();
    refreshPresence();
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

class $modify(PresencePlayLayer, PlayLayer) {
#if GEODE_COMP_GD_VERSION >= 22091
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        setScreen(Screen::Playing, level);
        return true;
    }
#else
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        setScreen(Screen::Playing, level);
        return true;
    }
#endif

    void resetLevel() {
        PlayLayer::resetLevel();
        if (s_state.screen != Screen::Playing) return;
        s_state.attempts = m_attempts;
        s_state.practice = m_isPracticeMode;
        refreshPresence(); // the Discord thread only sends the newest update every few seconds
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        s_state.completed = true;
        refreshPresence();
    }

    void onQuit() {
        PlayLayer::onQuit();
        setScreen(Screen::Menu);
    }
};

class $modify(PresenceEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        setScreen(Screen::Editing, level);
        return true;
    }
};

class $modify(PresenceEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        setScreen(Screen::EditorMenu, level);
        return true;
    }
};

class $modify(PresenceLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        setScreen(Screen::Viewing, level);
        return true;
    }
};

class $modify(PresenceBrowserLayer, LevelBrowserLayer) {
    bool init(GJSearchObject* search) {
        if (!LevelBrowserLayer::init(search)) return false;
        setScreen(Screen::Browsing);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Self-updater (same as the GD Gaming List mod)
// ---------------------------------------------------------------------------

#if GEODE_COMP_GD_VERSION >= 22091
static constexpr auto RELEASE_BASE = "https://github.com/pester33/Geode-DIscord/releases/download/latest-build-2.2091/";
#else
static constexpr auto RELEASE_BASE = "https://github.com/pester33/Geode-DIscord/releases/download/latest-build/";
#endif

static async::TaskHolder<web::WebResponse> s_updateTask;
static bool s_updateBusy = false;

static void whenScreenReady(std::function<void()> fn, int tries = 0) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    bool ready = scene && !typeinfo_cast<CCTransitionScene*>(scene);
    if (ready || tries > 600) {
        fn();
        return;
    }
    Loader::get()->queueInMainThread([fn = std::move(fn), tries]() mutable {
        whenScreenReady(std::move(fn), tries + 1);
    });
}

static void installUpdate(std::string version) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    auto url = fmt::format("{}{}.geode", RELEASE_BASE, Mod::get()->getID());
    log::info("[updater] Downloading {} from {}", version, url);
    auto loading = Notification::create("Downloading GD Discord Presence update...", NotificationIcon::Loading, 0.f);
    loading->show();

    web::WebRequest req;
    req.timeout(std::chrono::seconds(90));
    s_updateTask.spawn(req.get(url), [loading = Ref(loading), version](web::WebResponse res) {
        s_updateBusy = false;
        loading->hide();
        auto const& data = res.data();
        log::info("[updater] Download finished: HTTP {}, {} bytes", res.code(), data.size());
        if (!res.ok() || data.size() < 4 || data[0] != 'P' || data[1] != 'K') {
            Notification::create(fmt::format("Update failed to download (HTTP {})", res.code()), NotificationIcon::Error, 3.f)->show();
            return;
        }
        auto written = file::writeBinarySafe(Mod::get()->getPackagePath(), data);
        if (!written) {
            log::warn("[updater] Couldn't write the mod file: {}", written.unwrapErr());
            Notification::create("Update failed to install", NotificationIcon::Error, 3.f)->show();
            return;
        }
        whenScreenReady([version] {
            createQuickPopup(
                "Update installed",
                fmt::format("GD Discord Presence <cg>{}</c> is installed.\nRestart GD to use it.", version),
                "Later", "Restart",
                [](auto, bool restart) { if (restart) game::restart(true); }
            );
        });
    });
}

static void checkForUpdate(bool isRetry = false) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    auto url = fmt::format("{}version.txt", RELEASE_BASE);
    log::info("[updater] Checking {} (current {})", url, Mod::get()->getVersion().toVString());

    web::WebRequest req;
    req.timeout(std::chrono::seconds(20));
    s_updateTask.spawn(req.get(url), [isRetry](web::WebResponse res) {
        s_updateBusy = false;
        auto body = utils::string::trim(res.string().unwrapOr(""));
        log::info("[updater] version.txt: HTTP {}, body '{}'", res.code(), body.substr(0, 40));
        if (!res.ok()) {
            if (!isRetry) Loader::get()->queueInMainThread([] { checkForUpdate(true); });
            return;
        }
        auto latest = VersionInfo::parse(body);
        if (!latest) return;
        auto current = Mod::get()->getVersion();
        if (!(latest.unwrap() > current)) return;
        auto version = latest.unwrap().toVString();
        whenScreenReady([version, current] {
            createQuickPopup(
                "GD Discord Presence",
                fmt::format("A new version is out: <cg>{}</c> (you have {}).\nUpdate now?", version, current.toVString()),
                "Later", "Update",
                [version](auto, bool update) { if (update) installUpdate(version); }
            );
        });
    });
}

class $modify(PresenceMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        static bool firstTime = true;
        if (firstTime) {
            firstTime = false;
            loadGdgl();
            if (Mod::get()->getSettingValue<bool>("check-updates")) checkForUpdate();
        }
        setScreen(Screen::Menu);
        return true;
    }
};

// Re-send the presence right away when a setting changes
$on_mod(Loaded) {
    for (auto key : { "enabled", "show-level-name", "show-progress", "show-gdgl" }) {
        listenForSettingChanges<bool>(key, [](bool) {
            loadGdgl();
            refreshPresence();
        });
    }
    listenForSettingChanges<std::string>("client-id", [](std::string) { refreshPresence(); });
}
