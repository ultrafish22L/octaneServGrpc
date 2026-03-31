#include "categories/enum_fuzz.h"
#include "util/fuzz_values.h"
#include "util/connection.h"

#include "apirender.grpc.pb.h"
#include "apinodesystem_7.grpc.pb.h"
#include "apiprojectmanager.grpc.pb.h"

namespace fuzz {

TestCategory makeEnumFuzz(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "enum_fuzz";

    auto channel = getSharedChannel(addr);
    auto renderStub = std::shared_ptr<octaneapi::ApiRenderEngineService::Stub>(octaneapi::ApiRenderEngineService::NewStub(channel));
    auto nodeStub = std::shared_ptr<octaneapi::ApiNodeService::Stub>(octaneapi::ApiNodeService::NewStub(channel));
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(octaneapi::ApiProjectManagerService::NewStub(channel));

    // setClayMode — valid range 0-3
    std::vector<int32_t> outOfRange = {-1, 4, 1000, INT32_MIN, INT32_MAX};
    for (auto v : outOfRange) {
        cat.tests.push_back({"clayMode_" + std::to_string(v),
            "setClayMode with value=" + std::to_string(v),
            grpc::StatusCode::INVALID_ARGUMENT,
            [renderStub, v, timeoutMs]() {
                octaneapi::ApiRenderEngine::setClayModeRequest req;
                req.set_mode(static_cast<octaneapi::ClayMode>(v));
                google::protobuf::Empty resp;
                auto ctx = makeContext(timeoutMs);
                return renderStub->setClayMode(ctx.get(), req, &resp);
            }
        });
    }

    // setSubSampleMode — valid range 0-3
    for (auto v : outOfRange) {
        cat.tests.push_back({"subSampleMode_" + std::to_string(v),
            "setSubSampleMode with value=" + std::to_string(v),
            grpc::StatusCode::INVALID_ARGUMENT,
            [renderStub, v, timeoutMs]() {
                octaneapi::ApiRenderEngine::setSubSampleModeRequest req;
                req.set_mode(static_cast<octaneapi::SubSampleMode>(v));
                google::protobuf::Empty resp;
                auto ctx = makeContext(timeoutMs);
                return renderStub->setSubSampleMode(ctx.get(), req, &resp);
            }
        });
    }

    // setRenderPriority — valid range 0-2
    std::vector<int32_t> prioOOR = {-1, 3, 1000, INT32_MIN, INT32_MAX};
    for (auto v : prioOOR) {
        cat.tests.push_back({"renderPriority_" + std::to_string(v),
            "setRenderPriority with value=" + std::to_string(v),
            grpc::StatusCode::INVALID_ARGUMENT,
            [renderStub, v, timeoutMs]() {
                octaneapi::ApiRenderEngine::setRenderPriorityRequest req;
                req.set_priority(static_cast<octaneapi::ApiRenderEngine::RenderPriority>(v));
                google::protobuf::Empty resp;
                auto ctx = makeContext(timeoutMs);
                return renderStub->setRenderPriority(ctx.get(), req, &resp);
            }
        });
    }

    // create node with bogus NodeType
    std::vector<int32_t> badTypes = {0, -1, 9999, INT32_MAX, INT32_MIN};
    for (auto v : badTypes) {
        cat.tests.push_back({"create_nodeType_" + std::to_string(v),
            "create node with bogus type=" + std::to_string(v),
            grpc::StatusCode::INVALID_ARGUMENT,
            [projStub, nodeStub, v, timeoutMs]() {
                // Get root graph
                octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
                octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
                auto ctx1 = makeContext(timeoutMs);
                auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
                if (!s.ok()) return s;

                octaneapi::ApiNode::createRequest req;
                req.set_type(static_cast<octaneapi::NodeType>(v));
                auto* ref = req.mutable_ownergraph();
                ref->set_handle(rResp.result().handle());
                ref->set_type(octaneapi::ObjectRef::ApiNodeGraph);
                req.set_configurepins(false);
                octaneapi::ApiNode::createResponse resp;
                auto ctx2 = makeContext(timeoutMs);
                return nodeStub->create(ctx2.get(), req, &resp);
            }
        });
    }

    // Valid enum values should succeed
    for (int v = 0; v <= 3; ++v) {
        cat.tests.push_back({"clayMode_valid_" + std::to_string(v),
            "setClayMode with valid value=" + std::to_string(v),
            grpc::StatusCode::OK,
            [renderStub, v, timeoutMs]() {
                octaneapi::ApiRenderEngine::setClayModeRequest req;
                req.set_mode(static_cast<octaneapi::ClayMode>(v));
                google::protobuf::Empty resp;
                auto ctx = makeContext(timeoutMs);
                return renderStub->setClayMode(ctx.get(), req, &resp);
            }
        });
    }

    // Reset clay mode to 0
    cat.tests.push_back({"clayMode_reset", "reset clay mode to 0",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::setClayModeRequest req;
            req.set_mode(static_cast<octaneapi::ClayMode>(0));
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->setClayMode(ctx.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
