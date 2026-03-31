#include "categories/stream_abuse.h"
#include "util/connection.h"

#include "callbackstream.grpc.pb.h"
#include "livelink.grpc.pb.h"
#include "apiinfo.grpc.pb.h"

#include <thread>
#include <chrono>
#include <cmath>

namespace fuzz {

TestCategory makeStreamAbuse(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "stream_abuse";

    // Open callback stream, never read — backpressure test
    cat.tests.push_back({"callback_never_read",
        "open callback stream, never read (backpressure test)",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = octaneapi::StreamCallbackService::NewStub(ch);

            auto ctx = makeContext(timeoutMs);
            google::protobuf::Empty req;
            auto reader = stub->callbackChannel(ctx.get(), req);

            // Don't read — let it sit for 1s
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Cancel and close
            ctx->TryCancel();
            reader->Finish();

            // Verify server still works
            auto ch2 = makeChannel(addr);
            auto infoStub = octaneapi::ApiInfoService::NewStub(ch2);
            octaneapi::ApiInfo::octaneVersionRequest vReq;
            octaneapi::ApiInfo::octaneVersionResponse vResp;
            auto ctx2 = makeContext(timeoutMs);
            return infoStub->octaneVersion(ctx2.get(), vReq, &vResp);
        }
    });

    // 10 simultaneous callback streams
    cat.tests.push_back({"10_simultaneous_streams",
        "open 10 callback streams simultaneously",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            struct StreamCtx {
                std::unique_ptr<grpc::ClientContext> ctx;
                std::unique_ptr<grpc::ClientReader<octaneapi::StreamCallbackRequest>> reader;
            };
            std::vector<StreamCtx> streams;

            for (int i = 0; i < 10; ++i) {
                auto ch = makeChannel(addr);
                auto stub = octaneapi::StreamCallbackService::NewStub(ch);
                StreamCtx sc;
                sc.ctx = makeContext(timeoutMs);
                google::protobuf::Empty req;
                sc.reader = stub->callbackChannel(sc.ctx.get(), req);
                streams.push_back(std::move(sc));
            }

            // Let them all sit for 500ms
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Cancel all
            for (auto& sc : streams) {
                sc.ctx->TryCancel();
                sc.reader->Finish();
            }

            // Verify server
            auto ch = makeChannel(addr);
            auto infoStub = octaneapi::ApiInfoService::NewStub(ch);
            octaneapi::ApiInfo::octaneVersionRequest vReq;
            octaneapi::ApiInfo::octaneVersionResponse vResp;
            auto ctx = makeContext(timeoutMs);
            return infoStub->octaneVersion(ctx.get(), vReq, &vResp);
        }
    });

    // Open stream, read 1 event, close mid-stream
    cat.tests.push_back({"stream_read_then_close",
        "open callback stream, attempt 1 read, close mid-stream",
        grpc::StatusCode::OK,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = octaneapi::StreamCallbackService::NewStub(ch);

            auto ctx = makeContext(2000); // 2s timeout
            google::protobuf::Empty req;
            auto reader = stub->callbackChannel(ctx.get(), req);

            // Try to read one message (will likely timeout since no render active)
            octaneapi::StreamCallbackRequest msg;
            reader->Read(&msg); // May return false if no events

            ctx->TryCancel();
            reader->Finish();

            // Verify
            auto ch2 = makeChannel(addr);
            auto infoStub = octaneapi::ApiInfoService::NewStub(ch2);
            octaneapi::ApiInfo::octaneVersionRequest vReq;
            octaneapi::ApiInfo::octaneVersionResponse vResp;
            auto ctx2 = makeContext(timeoutMs);
            return infoStub->octaneVersion(ctx2.get(), vReq, &vResp);
        }
    });

    // StreamCamera with 0 messages (immediate finish) — may be UNIMPLEMENTED
    cat.tests.push_back({"streamCamera_zero",
        "StreamCamera with 0 messages — immediate finish",
        grpc::StatusCode::UNIMPLEMENTED,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = livelinkapi::LiveLinkService::NewStub(ch);

            auto ctx = makeContext(timeoutMs);
            livelinkapi::StreamStatus resp;
            auto writer = stub->StreamCamera(ctx.get(), &resp);

            // Immediately done
            writer->WritesDone();
            return writer->Finish();
        }
    });

    // StreamCamera with 1000 rapid messages — may be UNIMPLEMENTED
    cat.tests.push_back({"streamCamera_1000_rapid",
        "StreamCamera with 1000 rapid CameraState messages",
        grpc::StatusCode::UNIMPLEMENTED,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = livelinkapi::LiveLinkService::NewStub(ch);

            auto ctx = makeContext(timeoutMs);
            livelinkapi::StreamStatus resp;
            auto writer = stub->StreamCamera(ctx.get(), &resp);

            for (int i = 0; i < 1000; ++i) {
                livelinkapi::CameraState state;
                auto* pos = state.mutable_position();
                pos->set_x(static_cast<float>(i) * 0.01f);
                pos->set_y(1.0f);
                pos->set_z(5.0f);
                if (!writer->Write(state)) break;
            }

            writer->WritesDone();
            return writer->Finish();
        }
    });

    // StreamCamera with NaN values — may be UNIMPLEMENTED
    cat.tests.push_back({"streamCamera_nan",
        "StreamCamera with NaN values in CameraState",
        grpc::StatusCode::UNIMPLEMENTED,
        [addr, timeoutMs]() {
            auto ch = makeChannel(addr);
            auto stub = livelinkapi::LiveLinkService::NewStub(ch);

            auto ctx = makeContext(timeoutMs);
            livelinkapi::StreamStatus resp;
            auto writer = stub->StreamCamera(ctx.get(), &resp);

            livelinkapi::CameraState state;
            auto* pos = state.mutable_position();
            pos->set_x(std::numeric_limits<float>::quiet_NaN());
            pos->set_y(std::numeric_limits<float>::infinity());
            pos->set_z(-std::numeric_limits<float>::infinity());
            writer->Write(state);

            writer->WritesDone();
            return writer->Finish();
        }
    });

    return cat;
}

} // namespace fuzz
