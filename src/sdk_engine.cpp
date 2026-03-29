// SDK Engine — Octane Render SDK lifecycle management
// Adapted from octaneservermodule/src/common_octane/common_octserv.cpp

#include "sdk_engine.h"
#include "SERVKEY.h"
#include "octaneapi.h"
#include "octanegui/apimainwindow.h"
#include "util/callback_dispatcher.h"
#include "util/server_log.h"

#include <iostream>
#include <vector>
#include <cstring>

// The plugin type identifier — must match the plugin key names
#define PLUGIN_TYPE "PLORTEST"

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
    // Select key based on tier (we use T2 for Studio+)
    const uint8_t* PLUGIN_AUTH_KEY = PLUGIN_AUTH_KEY_WINDOWS_T2;

    if (PLUGIN_AUTH_KEY == nullptr) {
        return nullptr;
    }

    // Make a copy of the original key
    std::vector<uint8_t> randomizedKey(PLUGIN_AUTH_KEY, PLUGIN_AUTH_KEY + 64);

    // Invoke the API's function to randomize the key
    if (Octane::randomizePluginAuthenticationKey(randomizedKey.data(), randomizedKey.size())) {
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

    // Tier check — we're built for Studio+ (tier 2)
    if (Octane::ApiInfo::tierIdx() != 2) {
        std::cerr << "[OctaneServGrpc] ERROR: SDK tier mismatch. Expected tier 2 (Studio+)." << std::endl;
        return false;
    }

    // Core init — start the engine
    const char* error = nullptr;
    if (!Octane::apiMode_Shared_isStarted()) {
        if (!Octane::apiMode_Shared_start(pluginType, pluginAuthCallback, runDispatchLoop, &error)) {
            std::cerr << "[OctaneServGrpc] ERROR: apiMode_Shared_start failed: " << (error ? error : "unknown") << std::endl;
            return false;
        }
    }

    // Activate license
    if (!Octane::apiMode_isActivated()) {
        Octane::ActivationResult result = Octane::apiMode_activate(&error, false, nullptr, nullptr);
        (void)result;
    }
    if (!Octane::apiMode_isActivated()) {
        std::cerr << "[OctaneServGrpc] WARNING: License not activated: " << (error ? error : "unknown") << std::endl;
        // Continue anyway — demo mode may still work
    }

    std::cout << "[OctaneServGrpc] Octane SDK initialized. Version: " << Octane::ApiInfo::octaneVersion()
              << " (" << Octane::ApiInfo::octaneName() << ")" << std::endl;

    return true;
}

bool SdkEngine::Exit() {
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
    if (Octane::apiMode_Shared_isStarted()) {
        Octane::ApiNetRenderManager::openOctanePreferences();
    }
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
