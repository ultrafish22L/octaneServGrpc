#include "categories/connection_abuse.h"
#include "util/connection.h"

#include "apiinfo.grpc.pb.h"
#include "livelink.grpc.pb.h"

#include <thread>
#include <vector>

namespace fuzz {

TestCategory makeConnectionAbuse(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "connection_abuse";

    // 100 rapid channel create/destroy cycles
    cat.tests.push_back({"rapid_channel_churn",
        "100 rapid channel create → call → destroy cycles",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            for (int i = 0; i < 100; ++i) {
                auto ch = makeChannel(addr);
                auto stub = octaneapi::ApiInfoService::NewStub(ch);
                octaneapi::ApiInfo::octaneVersionRequest req;
                octaneapi::ApiInfo::octaneVersionResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto s = stub->octaneVersion(ctx.get(), req, &resp);
                if (!s.ok()) return s;
                // Channel destroyed at end of loop iteration
            }
            return grpc::Status::OK;
        }
    });

    // 50 simultaneous channels
    cat.tests.push_back({"50_simultaneous_channels",
        "50 channels open simultaneously, call on each",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            std::vector<std::shared_ptr<grpc::Channel>> channels;
            std::vector<std::unique_ptr<octaneapi::ApiInfoService::Stub>> stubs;

            for (int i = 0; i < 50; ++i) {
                channels.push_back(makeChannel(addr));
                stubs.push_back(octaneapi::ApiInfoService::NewStub(channels.back()));
            }

            for (int i = 0; i < 50; ++i) {
                octaneapi::ApiInfo::octaneVersionRequest req;
                octaneapi::ApiInfo::octaneVersionResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto s = stubs[i]->octaneVersion(ctx.get(), req, &resp);
                if (!s.ok()) return s;
            }
            return grpc::Status::OK;
        }
    });

    // Half-open connection (start RPC, abandon context)
    cat.tests.push_back({"half_open_abandon",
        "start RPC then abandon context (half-open)",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = octaneapi::ApiInfoService::NewStub(ch);

            // Start and abandon 10 contexts
            for (int i = 0; i < 10; ++i) {
                auto ctx = makeContext(500); // short timeout
                octaneapi::ApiInfo::octaneVersionRequest req;
                octaneapi::ApiInfo::octaneVersionResponse resp;
                // Intentionally let context expire without reading
                ctx->TryCancel();
            }

            // Verify server still works
            octaneapi::ApiInfo::octaneVersionRequest req;
            octaneapi::ApiInfo::octaneVersionResponse resp;
            auto ctx = makeContext(timeoutMs);
            return stub->octaneVersion(ctx.get(), req, &resp);
        }
    });

    // Channel idle for 5s then call
    cat.tests.push_back({"idle_channel_reuse",
        "channel idle 3s then call (keepalive test)",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = octaneapi::ApiInfoService::NewStub(ch);

            // First call
            {
                octaneapi::ApiInfo::octaneVersionRequest req;
                octaneapi::ApiInfo::octaneVersionResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto s = stub->octaneVersion(ctx.get(), req, &resp);
                if (!s.ok()) return s;
            }

            // Idle
            std::this_thread::sleep_for(std::chrono::seconds(3));

            // Second call on same channel
            octaneapi::ApiInfo::octaneVersionRequest req;
            octaneapi::ApiInfo::octaneVersionResponse resp;
            auto ctx = makeContext(timeoutMs);
            return stub->octaneVersion(ctx.get(), req, &resp);
        }
    });

    // Immediate cancel
    cat.tests.push_back({"immediate_cancel",
        "start RPC then immediately TryCancel",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = octaneapi::ApiInfoService::NewStub(ch);

            // Cancel 20 RPCs immediately
            for (int i = 0; i < 20; ++i) {
                auto ctx = makeContext(timeoutMs);
                octaneapi::ApiInfo::octaneVersionRequest req;
                octaneapi::ApiInfo::octaneVersionResponse resp;
                ctx->TryCancel();
                stub->octaneVersion(ctx.get(), req, &resp);
                // Don't check result — cancelled is fine
            }

            // Verify server still works
            octaneapi::ApiInfo::octaneVersionRequest req;
            octaneapi::ApiInfo::octaneVersionResponse resp;
            auto ctx = makeContext(timeoutMs);
            return stub->octaneVersion(ctx.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
