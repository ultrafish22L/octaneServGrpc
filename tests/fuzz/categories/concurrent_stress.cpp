#include "categories/concurrent_stress.h"
#include "util/connection.h"

#include "apiinfo.grpc.pb.h"
#include "apirender.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"
#include "livelink.grpc.pb.h"

#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iostream>

namespace fuzz {

static octaneapi::ObjectRef makeRef(uint64_t handle) {
    octaneapi::ObjectRef ref;
    ref.set_handle(handle);
    ref.set_type(octaneapi::ObjectRef::ApiItem);
    return ref;
}

// Countdown latch for synchronized thread start
class Latch {
public:
    explicit Latch(int count) : mCount(count) {}
    void countDown() {
        std::lock_guard<std::mutex> lk(mMtx);
        if (--mCount <= 0) mCv.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lk(mMtx);
        mCv.wait(lk, [this] { return mCount <= 0; });
    }
private:
    std::mutex mMtx;
    std::condition_variable mCv;
    int mCount;
};

TestCategory makeConcurrentStress(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "concurrent_stress";

    // 50 threads × 200 octaneVersion calls
    cat.tests.push_back({"concurrent_50x200_version",
        "50 threads × 200 octaneVersion calls",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            const int THREADS = 50, CALLS = 200;
            std::atomic<int> failures{0};
            std::vector<std::thread> threads;

            for (int t = 0; t < THREADS; ++t) {
                threads.emplace_back([&, addr, timeoutMs]() {
                    auto ch = makeChannel(addr);
                    auto stub = octaneapi::ApiInfoService::NewStub(ch);
                    for (int i = 0; i < CALLS; ++i) {
                        octaneapi::ApiInfo::octaneVersionRequest req;
                        octaneapi::ApiInfo::octaneVersionResponse resp;
                        auto ctx = makeContext(timeoutMs);
                        if (!stub->octaneVersion(ctx.get(), req, &resp).ok())
                            failures++;
                    }
                });
            }
            for (auto& t : threads) t.join();

            if (failures > 0)
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    std::to_string(failures.load()) + " failures in concurrent test");
            return grpc::Status::OK;
        }
    });

    // 50 threads × mixed read RPCs
    cat.tests.push_back({"concurrent_50_mixed_reads",
        "50 threads with mixed read RPCs",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            const int THREADS = 50, CALLS = 100;
            std::atomic<int> failures{0};
            std::vector<std::thread> threads;

            for (int t = 0; t < THREADS; ++t) {
                threads.emplace_back([&, addr, timeoutMs, t]() {
                    auto ch = makeChannel(addr);
                    auto infoStub = octaneapi::ApiInfoService::NewStub(ch);
                    auto renderStub = octaneapi::ApiRenderEngineService::NewStub(ch);
                    auto llStub = livelinkapi::LiveLinkService::NewStub(ch);

                    for (int i = 0; i < CALLS; ++i) {
                        grpc::Status s;
                        switch ((t + i) % 3) {
                            case 0: {
                                octaneapi::ApiInfo::octaneVersionRequest req;
                                octaneapi::ApiInfo::octaneVersionResponse resp;
                                auto ctx = makeContext(timeoutMs);
                                s = infoStub->octaneVersion(ctx.get(), req, &resp);
                                break;
                            }
                            case 1: {
                                octaneapi::ApiRenderEngine::getDeviceCountRequest req;
                                octaneapi::ApiRenderEngine::getDeviceCountResponse resp;
                                auto ctx = makeContext(timeoutMs);
                                s = renderStub->getDeviceCount(ctx.get(), req, &resp);
                                break;
                            }
                            case 2: {
                                livelinkapi::Empty req;
                                livelinkapi::ServVersionResponse resp;
                                auto ctx = makeContext(timeoutMs);
                                s = llStub->GetServVersion(ctx.get(), req, &resp);
                                break;
                            }
                        }
                        if (!s.ok()) failures++;
                    }
                });
            }
            for (auto& t : threads) t.join();

            if (failures > 0)
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    std::to_string(failures.load()) + " failures");
            return grpc::Status::OK;
        }
    });

    // 50 threads all probing handle=42 (mutex contention)
    cat.tests.push_back({"concurrent_50_handle_probe",
        "50 threads probing handle=42 (mutex contention on registry)",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            const int THREADS = 50, CALLS = 200;
            std::atomic<int> unexpected{0};
            std::vector<std::thread> threads;

            for (int t = 0; t < THREADS; ++t) {
                threads.emplace_back([&, addr, timeoutMs]() {
                    auto ch = makeChannel(addr);
                    auto stub = octaneapi::ApiItemService::NewStub(ch);
                    for (int i = 0; i < CALLS; ++i) {
                        octaneapi::ApiItem::isNodeRequest req;
                        *req.mutable_objectptr() = makeRef(42);
                        octaneapi::ApiItem::isNodeResponse resp;
                        auto ctx = makeContext(timeoutMs);
                        auto s = stub->isNode(ctx.get(), req, &resp);
                        if (s.error_code() != grpc::StatusCode::NOT_FOUND)
                            unexpected++;
                    }
                });
            }
            for (auto& t : threads) t.join();

            if (unexpected > 0)
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    std::to_string(unexpected.load()) + " unexpected responses");
            return grpc::Status::OK;
        }
    });

    // 25 readers + 25 writers (setClayMode / clayMode)
    cat.tests.push_back({"concurrent_rw_clayMode",
        "25 reader + 25 writer threads on clayMode",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            const int THREADS = 25, CALLS = 100;
            std::atomic<int> failures{0};
            std::vector<std::thread> threads;

            // Writers
            for (int t = 0; t < THREADS; ++t) {
                threads.emplace_back([&, addr, timeoutMs, t]() {
                    auto ch = makeChannel(addr);
                    auto stub = octaneapi::ApiRenderEngineService::NewStub(ch);
                    for (int i = 0; i < CALLS; ++i) {
                        octaneapi::ApiRenderEngine::setClayModeRequest req;
                        req.set_mode(static_cast<octaneapi::ClayMode>((t + i) % 4));
                        google::protobuf::Empty resp;
                        auto ctx = makeContext(timeoutMs);
                        if (!stub->setClayMode(ctx.get(), req, &resp).ok())
                            failures++;
                    }
                });
            }
            // Readers
            for (int t = 0; t < THREADS; ++t) {
                threads.emplace_back([&, addr, timeoutMs]() {
                    auto ch = makeChannel(addr);
                    auto stub = octaneapi::ApiRenderEngineService::NewStub(ch);
                    for (int i = 0; i < CALLS; ++i) {
                        octaneapi::ApiRenderEngine::clayModeRequest req;
                        octaneapi::ApiRenderEngine::clayModeResponse resp;
                        auto ctx = makeContext(timeoutMs);
                        if (!stub->clayMode(ctx.get(), req, &resp).ok())
                            failures++;
                    }
                });
            }
            for (auto& t : threads) t.join();

            // Reset clay mode
            {
                auto ch = makeChannel(addr);
                auto stub = octaneapi::ApiRenderEngineService::NewStub(ch);
                octaneapi::ApiRenderEngine::setClayModeRequest req;
                req.set_mode(static_cast<octaneapi::ClayMode>(0));
                google::protobuf::Empty resp;
                auto ctx = makeContext(timeoutMs);
                stub->setClayMode(ctx.get(), req, &resp);
            }

            if (failures > 0)
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    std::to_string(failures.load()) + " failures");
            return grpc::Status::OK;
        }
    });

    // 100-thread burst
    cat.tests.push_back({"concurrent_100_burst",
        "100 threads synchronized burst — one RPC each",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            const int THREADS = 100;
            std::atomic<int> failures{0};
            auto latch = std::make_shared<Latch>(THREADS);
            std::vector<std::thread> threads;

            for (int t = 0; t < THREADS; ++t) {
                threads.emplace_back([&, addr, timeoutMs, latch]() {
                    auto ch = makeChannel(addr);
                    auto stub = octaneapi::ApiInfoService::NewStub(ch);

                    // Wait for all threads to be ready
                    latch->countDown();
                    latch->wait();

                    // Fire!
                    octaneapi::ApiInfo::octaneVersionRequest req;
                    octaneapi::ApiInfo::octaneVersionResponse resp;
                    auto ctx = makeContext(timeoutMs);
                    if (!stub->octaneVersion(ctx.get(), req, &resp).ok())
                        failures++;
                });
            }
            for (auto& t : threads) t.join();

            if (failures > 0)
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    std::to_string(failures.load()) + " failures");
            return grpc::Status::OK;
        }
    });

    return cat;
}

} // namespace fuzz
