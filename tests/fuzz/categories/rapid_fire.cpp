#include "categories/rapid_fire.h"
#include "util/connection.h"

#include "apiinfo.grpc.pb.h"
#include "apirender.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"

#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>

namespace fuzz {

static octaneapi::ObjectRef makeRef(uint64_t handle) {
    octaneapi::ObjectRef ref;
    ref.set_handle(handle);
    ref.set_type(octaneapi::ObjectRef::ApiItem);
    return ref;
}

// Returns percentile value from sorted vector
static double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(p / 100.0 * (sorted.size() - 1));
    return sorted[std::min(idx, sorted.size() - 1)];
}

TestCategory makeRapidFire(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "rapid_fire";

    auto channel = getSharedChannel(addr);
    auto infoStub = std::shared_ptr<octaneapi::ApiInfoService::Stub>(octaneapi::ApiInfoService::NewStub(channel));
    auto renderStub = std::shared_ptr<octaneapi::ApiRenderEngineService::Stub>(octaneapi::ApiRenderEngineService::NewStub(channel));
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(octaneapi::ApiItemService::NewStub(channel));

    const int N = 10000;

    // 10K octaneVersion calls
    cat.tests.push_back({"rapid_octaneVersion",
        "10K sequential octaneVersion calls",
        grpc::StatusCode::OK,
        [infoStub, timeoutMs, N]() {
            std::vector<double> latencies;
            latencies.reserve(N);
            for (int i = 0; i < N; ++i) {
                octaneapi::ApiInfo::octaneVersionRequest req;
                octaneapi::ApiInfo::octaneVersionResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto t0 = std::chrono::steady_clock::now();
                auto s = infoStub->octaneVersion(ctx.get(), req, &resp);
                auto t1 = std::chrono::steady_clock::now();
                latencies.push_back(
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
                if (!s.ok()) return s;
            }
            std::sort(latencies.begin(), latencies.end());
            std::cout << "    octaneVersion x" << N
                      << ": p50=" << std::fixed << std::setprecision(2)
                      << percentile(latencies, 50)
                      << "ms p95=" << percentile(latencies, 95)
                      << "ms p99=" << percentile(latencies, 99)
                      << "ms max=" << latencies.back() << "ms\n";
            return grpc::Status::OK;
        }
    });

    // 10K getDeviceCount
    cat.tests.push_back({"rapid_getDeviceCount",
        "10K sequential getDeviceCount calls",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs, N]() {
            for (int i = 0; i < N; ++i) {
                octaneapi::ApiRenderEngine::getDeviceCountRequest req;
                octaneapi::ApiRenderEngine::getDeviceCountResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto s = renderStub->getDeviceCount(ctx.get(), req, &resp);
                if (!s.ok()) return s;
            }
            return grpc::Status::OK;
        }
    });

    // 10K isNode(handle=0) — guaranteed error path
    cat.tests.push_back({"rapid_isNode_error",
        "10K sequential isNode(handle=0) — error path stress",
        grpc::StatusCode::OK,
        [itemStub, timeoutMs, N]() {
            for (int i = 0; i < N; ++i) {
                octaneapi::ApiItem::isNodeRequest req;
                *req.mutable_objectptr() = makeRef(0);
                octaneapi::ApiItem::isNodeResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto s = itemStub->isNode(ctx.get(), req, &resp);
                // Expect INVALID_ARGUMENT, but we want to ensure no crash
                if (s.error_code() != grpc::StatusCode::INVALID_ARGUMENT)
                    return s;
            }
            return grpc::Status::OK;
        }
    });

    // Latency degradation test
    cat.tests.push_back({"rapid_degradation",
        "latency degradation check: p99(first 1K) vs p99(last 1K)",
        grpc::StatusCode::OK,
        [infoStub, timeoutMs, N]() {
            std::vector<double> latencies;
            latencies.reserve(N);
            for (int i = 0; i < N; ++i) {
                octaneapi::ApiInfo::octaneVersionRequest req;
                octaneapi::ApiInfo::octaneVersionResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto t0 = std::chrono::steady_clock::now();
                auto s = infoStub->octaneVersion(ctx.get(), req, &resp);
                auto t1 = std::chrono::steady_clock::now();
                latencies.push_back(
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
                if (!s.ok()) return s;
            }

            // Compare first 1K vs last 1K
            std::vector<double> first(latencies.begin(), latencies.begin() + 1000);
            std::vector<double> last(latencies.end() - 1000, latencies.end());
            std::sort(first.begin(), first.end());
            std::sort(last.begin(), last.end());

            double p99First = percentile(first, 99);
            double p99Last = percentile(last, 99);
            double ratio = (p99First > 0) ? p99Last / p99First : 1.0;

            std::cout << "    Degradation: p99 first=" << std::fixed
                      << std::setprecision(2) << p99First
                      << "ms last=" << p99Last
                      << "ms ratio=" << ratio << "x\n";

            if (ratio > 3.0) {
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    "latency degraded " + std::to_string(ratio) + "x");
            }
            return grpc::Status::OK;
        }
    });

    return cat;
}

} // namespace fuzz
