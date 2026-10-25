#include "shared/config/Config.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include "common/config/version/Version.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/util/Util.h"

namespace coop {

ClientConfig& Cfg() {
    static ClientConfig cfg;
    return cfg;
}

bool ClientConfig::Load(const std::wstring& coopDirectory) {
    coopDir = coopDirectory;
    gameDir = ModuleDir(nullptr);
    iniPath = coopDir + L"coop.ini";
    // SACOOP_CONFIG=<file> (environment) selects another config: several game copies on one PC,
    // test setups. The command line is avoided because modloader chokes on unknown arguments.
    wchar_t env[MAX_PATH];
    DWORD envLen = GetEnvironmentVariableW(L"SACOOP_CONFIG", env, MAX_PATH);
    bool absolute = envLen > 1 && (env[1] == L':' || (env[0] == L'\\' && env[1] == L'\\'));  // tests keep theirs aside
    if (envLen > 0 && envLen < MAX_PATH) iniPath = absolute ? std::wstring(env) : coopDir + env;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (_wcsicmp(argv[i], L"-coopcfg") == 0) iniPath = coopDir + argv[i + 1];
        LocalFree(argv);
    }
    Ini ini;
    if (!ini.Load(iniPath)) return false;

    nickname = SanitizeNickname(ini.Get("player", "name", nickname));
    if (nickname.size() < 2) nickname = "Player";
    english = ToLower(ini.Get("player", "language", "ru")) == "en";  // configs made before the switch were Russian

    autoConnect = ini.GetBool("connect", "auto_connect", autoConnect);
    host = ini.Get("connect", "host", host);
    port = ini.GetInt("connect", "port", kDefaultPort);
    password = ini.Get("connect", "password", "");
    wantsHost = ToLower(ini.Get("connect", "mode", "join")) == "host";
    loadSlot = ini.GetInt("connect", "load_slot", 0);
    friendlyFire = ini.GetBool("connect", "friendly_fire", friendlyFire);
    noCheats = ini.GetBool("connect", "no_cheats", noCheats);
    maxPlayers = std::clamp(ini.GetInt("connect", "max_players", maxPlayers), 2, 16);

    windowed = ini.GetBool("game", "windowed", windowed);
    borderless = ini.GetBool("game", "borderless", borderless);
    exclusiveFullscreen = ini.GetBool("game", "exclusive_fullscreen", exclusiveFullscreen);
    betterScope = ini.GetBool("game", "better_scope", betterScope);
    if (!windowed && !exclusiveFullscreen) borderless = true;  // full screen as a borderless window
    if (borderless) windowed = true;
    windowedStrict = ini.GetBool("game", "windowed_strict", windowedStrict);
    windowX = ini.GetInt("game", "window_x", windowX);
    windowY = ini.GetInt("game", "window_y", windowY);
    width = ini.GetInt("game", "width", 0);
    height = ini.GetInt("game", "height", 0);
    drawDistance = std::clamp(ini.GetFloat("game", "draw_distance", drawDistance), 0.5f, 4.0f);
    lodDistance = std::clamp(ini.GetFloat("game", "lod_distance", lodDistance), 0.5f, 3.0f);
    streamingMemoryMb = std::clamp(ini.GetInt("game", "streaming_memory", streamingMemoryMb), 0, 1024);
    // 0.4.0 draws as far as it can: a coop.ini written before it gets the most (once; /draw changes it after)
    if (ini.GetInt("game", "draw_settings", 0) < 2) {
        drawDistance = 4.0f;
        lodDistance = 3.0f;
        streamingMemoryMb = std::max(streamingMemoryMb, 512);
        ini.Set("game", "draw_distance", "4.0");
        ini.Set("game", "lod_distance", "3.0");
        ini.SetInt("game", "streaming_memory", streamingMemoryMb);
        ini.SetInt("game", "draw_settings", 2);
        ini.Save(iniPath);
    }
    windowTitle = ini.Get("game", "window_title", "");
    multiInstance = ini.GetBool("game", "multi_instance", multiInstance);
    runInBackground = ini.GetBool("game", "run_in_background", runInBackground);
    noPauseMenu = ini.GetBool("game", "no_pause_menu", noPauseMenu);
    userDir = ini.Get("game", "user_dir", "");
    skipIntro = ini.GetBool("game", "skip_intro", skipIntro);
    partnerBlip = ini.GetBool("game", "partner_blip", partnerBlip);
    nameTags = ini.GetBool("game", "name_tags", nameTags);

    voiceEnabled = ini.GetBool("voice", "enabled", voiceEnabled);
    voiceKey = ini.GetInt("voice", "push_to_talk_key", voiceKey);
    voiceAlwaysOn = ini.GetBool("voice", "always_on", voiceAlwaysOn);
    voiceVolume = ini.GetFloat("voice", "volume", voiceVolume);
    micGain = ini.GetFloat("voice", "mic_gain", micGain);
    voiceProximity = ini.GetBool("voice", "proximity", voiceProximity);
    micDevice = ini.GetInt("voice", "mic_device", micDevice);
    speakerDevice = ini.GetInt("voice", "speaker_device", speakerDevice);

    chatKey = ini.GetInt("keys", "chat", chatKey);
    passengerKey = ini.GetInt("keys", "enter_as_passenger", passengerKey);
    playerListKey = ini.GetInt("keys", "player_list", playerListKey);
    reviveKey = ini.GetInt("keys", "revive", reviveKey);

    debugOverlay = ini.GetBool("debug", "overlay", debugOverlay);
    cleanScreen = ini.GetBool("debug", "clean_screen", cleanScreen);
    verboseLog = ini.GetBool("debug", "verbose_log", verboseLog);
    firewallPrompt = ini.GetBool("debug", "firewall_prompt", firewallPrompt);
    serverData = ini.Get("debug", "server_data", serverData);
    while (!serverData.empty() && (serverData.back() == '\\' || serverData.back() == '/')) serverData.pop_back();
    if (serverData.empty()) serverData = "server-data";  // a trailing backslash would escape the quote around it
    return true;
}

}  // namespace coop
