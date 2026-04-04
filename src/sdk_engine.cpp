// SDK Engine — Octane Render SDK lifecycle management

#include "sdk_engine.h"
#include "SERVKEY.h"
#include "octaneapi.h"
#include "octanegui/apimainwindow.h"
#include "util/callback_dispatcher.h"
#include "util/server_log.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdio>

// The plugin type identifier — must match the plugin key names
#define PLUGIN_TYPE "PLORTEST"

// Register 'serv' log flag with the Octane log system.
// This runs at static init; the flag starts disabled until we call setFlag().
API_RLOG_DECLARE(serv, "serv", "gRPC server operations")

// Callback that forwards ServerLog lines to the Octane log window
static void octaneLogSink(const char* prefix, const char* service, const char* method, const char* detail) {
    char buf[1024]; // sufficient for "[prefix] Service.method detail" log format
    if (detail && detail[0]) {
        std::snprintf(buf, sizeof(buf), "%s %s.%s %s", prefix, service, method, detail);
    } else {
        std::snprintf(buf, sizeof(buf), "%s %s.%s", prefix, service, method);
    }
    API_RLOG(serv, "%s", buf);
}

// HMAC-SHA256 implementation for plugin authentication
// Minimal HMAC-SHA256 using Windows CNG (Cryptography Next Generation)
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static bool hmacSha256(const std::vector<uint8_t>& key, const char* message, std::vector<uint8_t>& output) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS status;
    DWORD hashLength = 0, resultLength = 0;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) return false;

    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLength, sizeof(hashLength), &resultLength, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    output.resize(hashLength);

    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    ULONG msgLen = (ULONG)strlen(message);
    status = BCryptHashData(hHash, (PUCHAR)message, msgLen, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    status = BCryptFinishHash(hHash, output.data(), hashLength, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return BCRYPT_SUCCESS(status);
}
#endif

namespace OctaneServ {

const uint8_t* pluginAuthCallback() {
    // Select key based on build tier
#ifdef OCTANE_DEMO_VERSION
    const uint8_t* PLUGIN_AUTH_KEY = PLUGIN_AUTH_KEY_WINDOWS_T0;
#else
    const uint8_t* PLUGIN_AUTH_KEY = PLUGIN_AUTH_KEY_WINDOWS_T2;
#endif

    if (PLUGIN_AUTH_KEY == nullptr) {
        return nullptr;
    }

    // Make a copy of the original key
    std::vector<uint8_t> randomizedKey(PLUGIN_AUTH_KEY, PLUGIN_AUTH_KEY + 64);

    // Invoke the API's function to randomize the key
    if (Octane::randomizePluginAuthenticationKey(randomizedKey.data(), randomizedKey.size())) {
        // Static so the returned .data() pointer outlives the call — SDK retains it.
        // Only called once during apiMode_Shared_start(); not thread-safe if called concurrently.
        static std::vector<uint8_t> hash;
        if (hmacSha256(randomizedKey, PLUGIN_TYPE, hash)) {
            return hash.data();
        }
    }
    return nullptr;
}

bool SdkEngine::Init(const char* pluginType, bool runDispatchLoop) {
    // Version check
    if (Octane::ApiInfo::octaneVersion() != OCTANE_VERSION) {
        std::cerr << "[OctaneServGrpc] ERROR: octane.dll version mismatch! Expected "
                  << OCTANE_VERSION << ", got " << Octane::ApiInfo::octaneVersion() << std::endl;
        return false;
    }

    // Core init — start the engine (must happen before tier/activation checks)
    const char* error = nullptr;
    if (!Octane::apiMode_Shared_isStarted()) {
        if (!Octane::apiMode_Shared_start(pluginType, pluginAuthCallback, runDispatchLoop, &error)) {
            std::cerr << "[OctaneServGrpc] ERROR: apiMode_Shared_start failed: " << (error ? error : "unknown") << std::endl;
            return false;
        }
    }

    // Tier check — verify SDK matches build configuration (after engine start)
#ifdef OCTANE_DEMO_VERSION
    if (!Octane::ApiInfo::isDemoVersion()) {
        std::cerr << "[OctaneServGrpc] ERROR: Built for demo but SDK is not demo version." << std::endl;
        Octane::apiMode_Shared_exit(nullptr);
        return false;
    }
#endif

    // Activate license
#ifndef OCTANE_DEMO_VERSION
    if (!Octane::apiMode_isActivated()) {
        Octane::ActivationResult result = Octane::apiMode_activate(&error, false, nullptr, nullptr);
        (void)result;
    }
    if (!Octane::apiMode_isActivated()) {
        std::cerr << "[OctaneServGrpc] WARNING: License not activated: " << (error ? error : "unknown") << std::endl;
        // Continue anyway — demo mode may still work
    }
#endif

    // Enable the 'serv' log flag and wire up the Octane log sink
    Octane::ApiLogManager::setFlag("serv", 1);
    ServerLog::instance().setExternalSink(octaneLogSink);

    API_RLOG(serv, "SDK initialized. Version: %d (%s)",
        Octane::ApiInfo::octaneVersion(), Octane::ApiInfo::octaneName());

    std::cout << "[OctaneServGrpc] Octane SDK initialized. Version: " << Octane::ApiInfo::octaneVersion()
              << " (" << Octane::ApiInfo::octaneName() << ")" << std::endl;

    return true;
}

bool SdkEngine::Exit() {
    API_RLOG(serv, "SDK shutting down");
    ServerLog::instance().setExternalSink(nullptr); // disconnect before SDK exits
    return Octane::apiMode_Shared_exit(nullptr);
}

bool SdkEngine::IsReady() {
    return Octane::apiMode_Shared_isStarted();
}

int SdkEngine::GetVersion() {
    return Octane::ApiInfo::octaneVersion();
}

std::string SdkEngine::GetName() {
    const char* name = Octane::ApiInfo::octaneName();
    return name ? std::string(name) : "";
}

void SdkEngine::OpenAuthWindow() {
#ifndef OCTANE_DEMO_VERSION
    if (Octane::apiMode_Shared_isStarted()) {
        Octane::apiMode_openAuthManagementDlg();
    }
#endif
}

void SdkEngine::OpenLogWindow() {
    if (Octane::apiMode_Shared_isStarted()) {
        Octane::ApiLogManager::openOctaneLogWindow();
    }
}

void SdkEngine::OpenPreferences() {
#ifndef OCTANE_DEMO_VERSION
    if (Octane::apiMode_Shared_isStarted()) {
        Octane::ApiNetRenderManager::openOctanePreferences();
    }
#endif
}

void SdkEngine::OpenDeviceSettings() {
    if (Octane::apiMode_Shared_isStarted()) {
        Octane::ApiRenderEngine::openDeviceSettings();
    }
}

void SdkEngine::OpenMainWindow() {
    if (Octane::apiMode_Shared_isStarted()) {
        auto* mainWin = Octane::ApiMainWindow::fetchOrCreateInstance();
        if (mainWin) mainWin->show();
    }
}

bool SdkEngine::IsActivated() {
#ifndef OCTANE_DEMO_VERSION
    return Octane::apiMode_isActivated();
#else
    return true;
#endif
}

// ── SDK Callback Wiring ─────────────────────────────────────────────────
// These static callbacks are registered with the SDK and push events into
// the CallbackDispatcher. They fire on SDK/render threads.

static Octane::ApiProjectManager::Observer sProjectObserver;
static bool sCallbacksRegistered = false;

static void onNewImageCB(const Octane::ApiArray<Octane::ApiRenderImage>&, void*) {
    ServerLog::instance().req("SDK", "onNewImage", "subs=" + std::to_string(CallbackDispatcher::Instance().SubscriberCount()));
    CallbackDispatcher::Instance().Broadcast({CallbackEventType::NewImage, 0});
}

static void onNewStatisticsCB(void*) {
    CallbackDispatcher::Instance().Broadcast({CallbackEventType::NewStatistics, 0});
}

static void onRenderFailureCB(void*) {
    CallbackDispatcher::Instance().Broadcast({CallbackEventType::RenderFailure, 0});
}

static void onProjectChangedCB(void*) {
    CallbackDispatcher::Instance().Broadcast({CallbackEventType::ProjectChanged, 0});
}

void SdkEngine::RegisterCallbacks() {
    if (sCallbacksRegistered) return;

    ServerLog::instance().req("SdkEngine", "RegisterCallbacks");

    // Render callbacks
    Octane::ApiRenderEngine::setOnNewImageCallback(onNewImageCB, nullptr);
    Octane::ApiRenderEngine::setOnNewStatisticsCallback(onNewStatisticsCB, nullptr);
    Octane::ApiRenderEngine::setOnRenderFailureCallback(onRenderFailureCB, nullptr);

    // Project manager observer
    sProjectObserver.mOnProjectNew = onProjectChangedCB;
    sProjectObserver.mUserData = nullptr;
    Octane::ApiProjectManager::addObserver(sProjectObserver);

    sCallbacksRegistered = true;
    ServerLog::instance().res("SdkEngine", "RegisterCallbacks",
        "image+statistics+failure+project callbacks registered");
}

void SdkEngine::UnregisterCallbacks() {
    if (!sCallbacksRegistered) return;

    ServerLog::instance().req("SdkEngine", "UnregisterCallbacks");

    // Clear render callbacks (pass nullptr)
    Octane::ApiRenderEngine::setOnNewImageCallback(nullptr, nullptr);
    Octane::ApiRenderEngine::setOnNewStatisticsCallback(nullptr, nullptr);
    Octane::ApiRenderEngine::setOnRenderFailureCallback(nullptr, nullptr);

    // Remove project observer
    Octane::ApiProjectManager::removeObserver(sProjectObserver);

    sCallbacksRegistered = false;
    ServerLog::instance().res("SdkEngine", "UnregisterCallbacks", "all callbacks unregistered");
}

} // namespace OctaneServ
