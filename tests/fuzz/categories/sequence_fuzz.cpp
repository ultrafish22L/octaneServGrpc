#include "categories/sequence_fuzz.h"
#include "util/connection.h"

#include "apirender.grpc.pb.h"
#include "apiprojectmanager.grpc.pb.h"
#include "apinodesystem_6.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"
#include "livelink.grpc.pb.h"
#include "apiinfo.grpc.pb.h"

namespace fuzz {

static octaneapi::ObjectRef makeRef(uint64_t handle,
    octaneapi::ObjectRef::ObjectType type = octaneapi::ObjectRef::ApiItem) {
    octaneapi::ObjectRef ref;
    ref.set_handle(handle);
    ref.set_type(type);
    return ref;
}

TestCategory makeSequenceFuzz(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "sequence_fuzz";

    auto channel = getSharedChannel(addr);
    auto renderStub = std::shared_ptr<octaneapi::ApiRenderEngineService::Stub>(octaneapi::ApiRenderEngineService::NewStub(channel));
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(octaneapi::ApiProjectManagerService::NewStub(channel));
    auto graphStub = std::shared_ptr<octaneapi::ApiNodeGraphService::Stub>(octaneapi::ApiNodeGraphService::NewStub(channel));
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(octaneapi::ApiItemService::NewStub(channel));
    auto infoStub = std::shared_ptr<octaneapi::ApiInfoService::Stub>(octaneapi::ApiInfoService::NewStub(channel));
    auto llStub = std::shared_ptr<livelinkapi::LiveLinkService::Stub>(livelinkapi::LiveLinkService::NewStub(channel));

    // continueRendering before any setup
    cat.tests.push_back({"continueRendering_cold",
        "continueRendering before any scene setup",
        grpc::StatusCode::FAILED_PRECONDITION,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::continueRenderingRequest req;
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->continueRendering(ctx.get(), req, &resp);
        }
    });

    // saveProject with no project loaded
    cat.tests.push_back({"saveProject_cold", "saveProject with nothing loaded",
        grpc::StatusCode::OK, // May succeed with empty/default save
        [projStub, timeoutMs]() {
            octaneapi::ApiProjectManager::saveProjectAsRequest req;
            req.set_path("C:\\temp\\fuzz_test_save.ocs");
            octaneapi::ApiProjectManager::saveProjectAsResponse resp;
            auto ctx = makeContext(timeoutMs);
            return projStub->saveProjectAs(ctx.get(), req, &resp);
        }
    });

    // grabRenderResult before render
    cat.tests.push_back({"grabRenderResult_cold",
        "grabRenderResult before render — should return result=false",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::grabRenderResultRequest req;
            octaneapi::ApiRenderEngine::grabRenderResultResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->grabRenderResult(ctx.get(), req, &resp);
        }
    });

    // releaseRenderResult without prior grab
    cat.tests.push_back({"releaseRenderResult_cold",
        "releaseRenderResult without prior grab — no-op OK",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::releaseRenderResultRequest req;
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->releaseRenderResult(ctx.get(), req, &resp);
        }
    });

    // getRenderStatistics before render
    cat.tests.push_back({"getRenderStatistics_cold",
        "getRenderStatistics before render — zeroes OK",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::getRenderStatisticsRequest req;
            octaneapi::ApiRenderEngine::getRenderStatisticsResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->getRenderStatistics(ctx.get(), req, &resp);
        }
    });

    // setRenderTargetNode(0) — clears RT
    cat.tests.push_back({"setRT_zero", "setRenderTargetNode(0) — clear RT",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::setRenderTargetNodeRequest req;
            req.mutable_targetnode()->set_handle(0);
            octaneapi::ApiRenderEngine::setRenderTargetNodeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->setRenderTargetNode(ctx.get(), req, &resp);
        }
    });

    // getSceneBounds with empty scene
    cat.tests.push_back({"getSceneBounds_empty",
        "getSceneBounds with empty scene — result=false",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::getSceneBoundsRequest req;
            octaneapi::ApiRenderEngine::getSceneBoundsResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->getSceneBounds(ctx.get(), req, &resp);
        }
    });

    // copyItemTree with invalid handles
    cat.tests.push_back({"copyItemTree_invalid",
        "copyItemTree with invalid handles",
        grpc::StatusCode::NOT_FOUND,
        [graphStub, timeoutMs]() {
            octaneapi::ApiNodeGraph::copyItemTreeRequest req;
            *req.mutable_objectptr() = makeRef(9999, octaneapi::ObjectRef::ApiNodeGraph);
            *req.mutable_rootitem() = makeRef(8888);
            octaneapi::ApiNodeGraph::copyItemTreeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return graphStub->copyItemTree(ctx.get(), req, &resp);
        }
    });

    // Random rapid sequence of safe read RPCs
    cat.tests.push_back({"rapid_mixed_reads",
        "20 mixed read RPCs in rapid sequence",
        grpc::StatusCode::OK,
        [infoStub, renderStub,
         llStub, timeoutMs]() {
            grpc::Status last;
            for (int i = 0; i < 20; ++i) {
                switch (i % 4) {
                    case 0: {
                        octaneapi::ApiInfo::octaneVersionRequest req;
                        octaneapi::ApiInfo::octaneVersionResponse resp;
                        auto ctx = makeContext(timeoutMs);
                        last = infoStub->octaneVersion(ctx.get(), req, &resp);
                        break;
                    }
                    case 1: {
                        octaneapi::ApiRenderEngine::getDeviceCountRequest req;
                        octaneapi::ApiRenderEngine::getDeviceCountResponse resp;
                        auto ctx = makeContext(timeoutMs);
                        last = renderStub->getDeviceCount(ctx.get(), req, &resp);
                        break;
                    }
                    case 2: {
                        octaneapi::ApiRenderEngine::isCompilingRequest req;
                        octaneapi::ApiRenderEngine::isCompilingResponse resp;
                        auto ctx = makeContext(timeoutMs);
                        last = renderStub->isCompiling(ctx.get(), req, &resp);
                        break;
                    }
                    case 3: {
                        livelinkapi::Empty req;
                        livelinkapi::CameraState resp;
                        auto ctx = makeContext(timeoutMs);
                        last = llStub->GetCamera(ctx.get(), req, &resp);
                        break;
                    }
                }
                if (!last.ok()) return last;
            }
            return last;
        }
    });

    // findNodes with type=0 (NT_UNKNOWN) — potentially large result
    cat.tests.push_back({"findNodes_unknown_type",
        "findNodes(type=0) on root graph",
        grpc::StatusCode::OK,
        [projStub, graphStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            octaneapi::ApiNodeGraph::findNodesRequest req;
            *req.mutable_objectptr() = makeRef(rResp.result().handle(),
                octaneapi::ObjectRef::ApiNodeGraph);
            req.set_type(static_cast<octaneapi::NodeType>(0));
            req.set_recurse(false);
            octaneapi::ApiNodeGraph::findNodesResponse resp;
            auto ctx2 = makeContext(timeoutMs);
            return graphStub->findNodes(ctx2.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
