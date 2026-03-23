#pragma once
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
    static ServerLog& instance() {
        static ServerLog s;
        return s;
    }

    void init(const std::string& exePath, LogLevel level = LogLevel::Debug) {
        std::lock_guard<std::mutex> lock(mMutex);
        mLevel = level;
        if (mLevel == LogLevel::Off) return;

        // Place log file next to the exe
        std::string dir = exePath.substr(0, exePath.find_last_of("\\/"));
        std::string logPath = dir + "/log_serv.log";

        mFile.open(logPath, std::ios::app);
        if (!mFile.is_open()) {
            std::cerr << "[OctaneServGrpc] WARNING: Could not open " << logPath << " for logging" << std::endl;
            mLevel = LogLevel::Off;
            return;
        }

        mFile << "=== OctaneServGrpc Log started " << timestamp() << " (level: " << levelName() << ") ===" << std::endl;
        std::cout << "[OctaneServGrpc] Logging to " << logPath << " (level: " << levelName() << ")" << std::endl;
    }

    void log(const char* prefix, const std::string& service, const std::string& method,
             const std::string& detail = "") {
        if (mLevel == LogLevel::Off) return;

        bool isError = (prefix[0] == 'E'); // ERR

        // Level filtering — same logic as client
        if (mLevel == LogLevel::Warn && !isError) return;
        if (mLevel == LogLevel::Info && !isError && !sMutatingMethods.count(method)) return;
        if (mLevel == LogLevel::Debug && !isError &&
            !sMutatingMethods.count(method) && !sDebugMethods.count(method)) return;
        // Verbose: log everything

        std::lock_guard<std::mutex> lock(mMutex);
        if (!mFile.is_open()) return;

        std::string ts = timestamp();
        mFile << "[" << ts << "]  " << prefix << " " << service << "." << method;
        if (!detail.empty()) {
            // Truncate at 800 chars like client
            if (detail.size() > 800)
                mFile << " " << detail.substr(0, 800) << "...";
            else
                mFile << " " << detail;
        }
        mFile << "\n";
        mFile.flush(); // Flush every line for real-time tailing
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

    std::string levelName() {
        switch (mLevel) {
            case LogLevel::Verbose: return "verbose";
            case LogLevel::Debug:   return "debug";
            case LogLevel::Info:    return "info";
            case LogLevel::Warn:    return "warn";
            case LogLevel::Off:     return "off";
        }
        return "unknown";
    }

    std::mutex mMutex;
    std::ofstream mFile;
    LogLevel mLevel = LogLevel::Debug;

    // Mirror the client's method sets for consistent filtering
    static const std::unordered_set<std::string> sMutatingMethods;
    static const std::unordered_set<std::string> sDebugMethods;
};

// Convenience macro for services — logs REQ on entry, RES/ERR on exit
#define SERV_LOG_RPC(service, method) \
    OctaneServ::ServerLog::instance().req(service, method); \
    auto _servLogGuard = OctaneServ::detail::RpcLogGuard(service, method)

namespace detail {
    // RAII guard that logs RES on success, ERR on exception
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
