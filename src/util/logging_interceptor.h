#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// gRPC Server Interceptor — logs every incoming RPC call to log_serv.log
//
// Automatically logs REQ/RES/ERR for ALL services without per-method code.
// Parses service.method from the gRPC method string (/package.Service/Method).
//
// Also acts as the SDK readiness guard: if the SDK is not initialized,
// all RPCs are rejected with UNAVAILABLE before reaching service code.
// ═══════════════════════════════════════════════════════════════════════════

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_interceptor.h>
#include "server_log.h"
#include "../sdk_engine.h"
#include <string>
#include <chrono>

namespace OctaneServ {

class LoggingInterceptor : public grpc::experimental::Interceptor {
public:
    explicit LoggingInterceptor(grpc::experimental::ServerRpcInfo* info) {
        // Parse method string: "/octaneapi.ApiInfoService/octaneVersion"
        if (info && info->method()) {
            std::string full = info->method();
            if (!full.empty() && full[0] == '/') full = full.substr(1);
            auto pos = full.rfind('/');
            if (pos != std::string::npos) {
                mService = full.substr(0, pos);
                mMethod = full.substr(pos + 1);
                // Strip package prefix for readability
                auto dot = mService.rfind('.');
                if (dot != std::string::npos)
                    mService = mService.substr(dot + 1);
            } else {
                mService = "unknown";
                mMethod = full;
            }
        }
    }

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
        if (methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::POST_RECV_INITIAL_METADATA)) {
            mStart = std::chrono::steady_clock::now();
            ServerLog::instance().req(mService, mMethod);

            // SDK readiness guard — reject all RPCs if SDK is not ready.
            // This prevents crashes from calling SDK functions before init.
            if (!SdkEngine::IsReady()) {
                std::string msg = mService + "." + mMethod +
                    ": SDK not ready (engine not initialized or not started)";
                ServerLog::instance().err(mService, mMethod, msg);
                // Hijack the RPC and send UNAVAILABLE back to client
                methods->FailHijackedSendMessage();
                methods->ModifySendStatus(
                    grpc::Status(grpc::StatusCode::UNAVAILABLE, msg));
                methods->Hijack();
                return;
            }
        }

        if (methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::PRE_SEND_STATUS)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - mStart).count();

            auto status = methods->GetSendStatus();
            std::string detail = std::to_string(elapsed) + "ms";

            if (status.ok()) {
                ServerLog::instance().res(mService, mMethod, detail);
            } else {
                detail += " code=" + std::to_string(status.error_code());
                if (!status.error_message().empty())
                    detail += " " + status.error_message();
                ServerLog::instance().err(mService, mMethod, detail);
            }
        }

        methods->Proceed();
    }

private:
    std::string mService = "unknown";
    std::string mMethod = "unknown";
    std::chrono::steady_clock::time_point mStart;
};

class LoggingInterceptorFactory : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateServerInterceptor(
            grpc::experimental::ServerRpcInfo* info) override {
        return new LoggingInterceptor(info);
    }
};

} // namespace OctaneServ
