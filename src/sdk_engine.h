#pragma once
// SDK Engine — Octane Render SDK lifecycle management
// Adapted from octaneservermodule/src/common_octane/common_octserv.cpp

#include <string>
#include <cstdint>

namespace OctaneServ {

class SdkEngine {
public:
    /// Initialize the Octane Render SDK.
    /// Calls apiMode_Shared_start() and apiMode_activate().
    /// @param pluginType  Plugin type string (e.g. "PLORTEST")
    /// @param runDispatchLoop  Whether to run the message dispatch loop in a thread
    /// @return true if initialization and activation succeeded
    static bool Init(const char* pluginType, bool runDispatchLoop = true);

    /// Shut down the Octane Render SDK.
    /// Calls apiMode_Shared_exit().
    static bool Exit();

    /// Check if the SDK is initialized and activated.
    static bool IsReady();

    /// Get the Octane version number.
    static int GetVersion();

    /// Get the Octane product name string.
    static std::string GetName();

    /// Open the Octane authentication/license management window.
    static void OpenAuthWindow();

    /// Open the Octane log window.
    static void OpenLogWindow();

    /// Open Octane preferences dialog.
    static void OpenPreferences();

    /// Open GPU device settings dialog.
    static void OpenDeviceSettings();

    /// Open the full Octane Standalone main window.
    static void OpenMainWindow();

    /// Check if license is activated.
    static bool IsActivated();

    /// Register SDK callbacks (new image, statistics, failure, project change).
    /// Call after Init() succeeds. Pushes events to CallbackDispatcher.
    static void RegisterCallbacks();

    /// Unregister SDK callbacks. Call before Exit().
    static void UnregisterCallbacks();
};

/// Plugin authentication callback — returns HMAC-SHA256 auth code.
/// Called by apiMode_Shared_start() internally.
const uint8_t* pluginAuthCallback();

} // namespace OctaneServ
