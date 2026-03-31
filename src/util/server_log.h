#pragma once
#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#endif
// ═══════════════════════════════════════════════════════════════════════════
// OctaneServGrpc Logging
//
// Level-based file logging mirroring the client's log_grpc.log format.
// Output: log_serv.log (next to exe)
//
// Levels (set via --log-level or SERV_LOG_LEVEL env):
//   verbose  — log ALL gRPC calls (full firehose)
//   debug    — mutating + lifecycle + curated reads (DEFAULT)
//   info     — mutating + lifecycle calls only + errors
//   warn     — errors only
//   off      — disable
//
// Format matches client side for easy side-by-side diff:
//   [HH:MM:SS.mmm]  REQ ServiceName.methodName {json...}
//   [HH:MM:SS.mmm]  RES ServiceName.methodName {json...}
//   [HH:MM:SS.mmm]  ERR ServiceName.methodName {error...}
// ═══════════════════════════════════════════════════════════════════════════

#include <string>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace OctaneServ {

enum class LogLevel { Verbose, Debug, Info, Warn, Off };

class ServerLog {
public:
    /// Callback type for forwarding log lines to an external system (e.g. Octane log window)
    using ExternalLogFunc = void(*)(const char* prefix, const char* service, const char* method, const char* detail);

    static ServerLog& instance() {
        static ServerLog s;
        return s;
    }

    /// Set an external log sink (called for every line that passes level filtering)
    void setExternalSink(ExternalLogFunc func) { mExternalSink.store(func); }

    void init(const std::string& exePath, LogLevel level = LogLevel::Off) {
        std::lock_guard<std::mutex> lock(mMutex);
        mLevel = level;
        if (mLevel == LogLevel::Off) return;

        // Place log file next to the exe
        std::string dir;
        auto sep = exePath.find_last_of("\\/");
        if (sep != std::string::npos) {
            dir = exePath.substr(0, sep);
        } else {
            // No directory in path — resolve from exe module path
#ifdef _WIN32
            char modPath[MAX_PATH];
            if (GetModuleFileNameA(NULL, modPath, MAX_PATH)) {
                std::string mp(modPath);
                auto msep = mp.find_last_of("\\/");
                dir = (msep != std::string::npos) ? mp.substr(0, msep) : ".";
            } else {
                dir = ".";
            }
#else
            dir = ".";
#endif
        }
        mLogPath = dir + "/log_serv.log";

        mFile.open(mLogPath, std::ios::app);
        if (!mFile.is_open()) {
            std::cerr << "[OctaneServGrpc] WARNING: Could not open " << mLogPath << " for logging" << std::endl;
            mLevel = LogLevel::Off;
            return;
        }

        mFile << "=== OctaneServGrpc Log started " << timestamp() << " (level: " << levelName() << ") ===" << std::endl;
        std::cout << "[OctaneServGrpc] Logging to " << mLogPath << " (level: " << levelName() << ")" << std::endl;
    }

    void log(const char* prefix, const std::string& service, const std::string& method,
             const std::string& detail = "") {
        bool isError = (prefix[0] == 'E'); // ERR
        bool isReq = (prefix[0] == 'R' && prefix[1] == 'E' && prefix[2] == 'Q'); // REQ — verbose only

        // REQ logs are verbose-only (suppressed at all other levels, both file and sink)
        if (isReq && mLevel != LogLevel::Verbose) return;

        // Forward to Octane log window via external sink (independent of file log level).
        // Policy: errors always, RES for mutating/lifecycle/debug reads, REQ only at verbose.
        {
            bool forwardToSink = isError
                || sMutatingMethods.count(method)
                || sDebugMethods.count(method)
                || mLevel == LogLevel::Verbose;

            if (forwardToSink) {
                auto sink = mExternalSink.load();
                if (sink) {
                    sink(prefix, service.c_str(), method.c_str(), detail.c_str());
                }
            }
        }

        // File logging — skip if disabled
        if (mLevel == LogLevel::Off) return;

        // Level filtering
        if (mLevel == LogLevel::Warn && !isError) return;
        if (mLevel == LogLevel::Info && !isError && !sMutatingMethods.count(method)) return;
        if (mLevel == LogLevel::Debug && !isError &&
            !sMutatingMethods.count(method) && !sDebugMethods.count(method)) return;
        // Verbose: log everything

        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mFile.is_open()) {
                std::string ts = timestamp();
                mFile << "[" << ts << "]  " << prefix << " " << service << "." << method;
                if (!detail.empty()) {
                    // Truncate long details (e.g. base64 image data) to keep log readable
                    if (detail.size() > 800)
                        mFile << " " << detail.substr(0, 800) << "...";
                    else
                        mFile << " " << detail;
                }
                mFile << "\n";
                mFile.flush();
            }
        }
    }

    void req(const std::string& service, const std::string& method, const std::string& detail = "") {
        log("REQ", service, method, detail);
    }
    void res(const std::string& service, const std::string& method, const std::string& detail = "") {
        log("RES", service, method, detail);
    }
    void err(const std::string& service, const std::string& method, const std::string& detail = "") {
        log("ERR", service, method, detail);
    }

    LogLevel level() const { return mLevel; }
    const std::string& logPath() const { return mLogPath; }

    std::string levelName() const {
        switch (mLevel) {
            case LogLevel::Verbose: return "verbose";
            case LogLevel::Debug:   return "debug";
            case LogLevel::Info:    return "info";
            case LogLevel::Warn:    return "warn";
            case LogLevel::Off:     return "off";
        }
        return "unknown";
    }

    static LogLevel parseLevel(const std::string& s) {
        if (s == "verbose") return LogLevel::Verbose;
        if (s == "debug")   return LogLevel::Debug;
        if (s == "info")    return LogLevel::Info;
        if (s == "warn")    return LogLevel::Warn;
        if (s == "off")     return LogLevel::Off;
        return LogLevel::Debug;
    }

private:
    ServerLog() = default;

    std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    std::mutex mMutex;
    std::ofstream mFile;
    std::string mLogPath;
    LogLevel mLevel = LogLevel::Off;
    std::atomic<ExternalLogFunc> mExternalSink{nullptr};

    // Mirror the client's method sets for consistent filtering
    static const std::unordered_set<std::string> sMutatingMethods;
    static const std::unordered_set<std::string> sDebugMethods;
};

// RAII logging macro for RPC methods. Logs REQ on construction, then logs
// RES on .success() or ERR on destruction without .success(). Use in services
// that need manual control over log timing (e.g. StreamCallbackService).
#define SERV_LOG_RPC(service, method) \
    OctaneServ::ServerLog::instance().req(service, method); \
    auto _servLogGuard = OctaneServ::detail::RpcLogGuard(service, method)

namespace detail {
    // RAII guard: logs RES on .success(), ERR on destruction without .success().
    // Ensures every RPC entry gets a matching exit log even on exception paths.
    struct RpcLogGuard {
        std::string svc, meth;
        bool ok = false;
        RpcLogGuard(const std::string& s, const std::string& m) : svc(s), meth(m) {}
        void success(const std::string& detail = "") {
            ok = true;
            ServerLog::instance().res(svc, meth, detail);
        }
        void error(const std::string& detail) {
            ok = true;
            ServerLog::instance().err(svc, meth, detail);
        }
        ~RpcLogGuard() {
            if (!ok) ServerLog::instance().err(svc, meth, "unhandled");
        }
    };
}

} // namespace OctaneServ
