#include "categories/state_machine.h"
#include "util/connection.h"

#include "apirender.grpc.pb.h"
#include "apiprojectmanager.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"
#include "apinodesystem_7.grpc.pb.h"

namespace fuzz {

static octaneapi::ObjectRef makeRef(uint64_t handle,
    octaneapi::ObjectRef::ObjectType type = octaneapi::ObjectRef::ApiItem) {
    octaneapi::ObjectRef ref;
    ref.set_handle(handle);
    ref.set_type(type);
    return ref;
}

TestCategory makeStateMachine(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "state_machine";

    auto channel = getSharedChannel(addr);
    auto renderStub = std::shared_ptr<octaneapi::ApiRenderEngineService::Stub>(octaneapi::ApiRenderEngineService::NewStub(channel));
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(octaneapi::ApiProjectManagerService::NewStub(channel));
    auto nodeStub = std::shared_ptr<octaneapi::ApiNodeService::Stub>(octaneapi::ApiNodeService::NewStub(channel));
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(octaneapi::ApiItemService::NewStub(channel));

    // resetProject when idle — should be OK
    cat.tests.push_back({"resetProject_idle", "resetProject when no activity",
        grpc::StatusCode::OK,
        [projStub, timeoutMs]() {
            octaneapi::ApiProjectManager::resetProjectRequest req;
            req.set_suppressui(true);
            octaneapi::ApiProjectManager::resetProjectResponse resp;
            auto ctx = makeContext(timeoutMs);
            return projStub->resetProject(ctx.get(), req, &resp);
        }
    });

    // stopRendering when not rendering — no-op OK
    cat.tests.push_back({"stopRendering_idle", "stopRendering when not rendering",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::stopRenderingRequest req;
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->stopRendering(ctx.get(), req, &resp);
        }
    });

    // continueRendering with no RT — FAILED_PRECONDITION
    cat.tests.push_back({"continueRendering_noRT",
        "continueRendering with no render target set",
        grpc::StatusCode::FAILED_PRECONDITION,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::continueRenderingRequest req;
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->continueRendering(ctx.get(), req, &resp);
        }
    });

    // restartRendering with no RT
    cat.tests.push_back({"restartRendering_noRT",
        "restartRendering with no render target",
        grpc::StatusCode::FAILED_PRECONDITION,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::restartRenderingRequest req;
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->restartRendering(ctx.get(), req, &resp);
        }
    });

    // Create node, destroy, read name → NOT_FOUND
    cat.tests.push_back({"create_destroy_read",
        "create node → destroy → read name (stale)",
        grpc::StatusCode::NOT_FOUND,
        [projStub, nodeStub,
         itemStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            octaneapi::ApiNode::createRequest cReq;
            cReq.set_type(static_cast<octaneapi::NodeType>(89));
            *cReq.mutable_ownergraph() = makeRef(rResp.result().handle(),
                octaneapi::ObjectRef::ApiNodeGraph);
            cReq.set_configurepins(false);
            octaneapi::ApiNode::createResponse cResp;
            auto ctx2 = makeContext(timeoutMs);
            s = nodeStub->create(ctx2.get(), cReq, &cResp);
            if (!s.ok()) return s;
            uint64_t h = cResp.result().handle();

            // Destroy
            octaneapi::ApiItem::destroyRequest dReq;
            *dReq.mutable_objectptr() = makeRef(h);
            google::protobuf::Empty dResp;
            auto ctx3 = makeContext(timeoutMs);
            itemStub->destroy(ctx3.get(), dReq, &dResp);

            // Read name of destroyed node
            octaneapi::ApiItem::nameRequest nReq;
            *nReq.mutable_objectptr() = makeRef(h);
            octaneapi::ApiItem::nameResponse nResp;
            auto ctx4 = makeContext(timeoutMs);
            return itemStub->name(ctx4.get(), nReq, &nResp);
        }
    });

    // resetProject invalidates old graph handle
    cat.tests.push_back({"reset_invalidates_handles",
        "resetProject then use old graph handle → NOT_FOUND",
        grpc::StatusCode::NOT_FOUND,
        [projStub, itemStub, timeoutMs]() {
            // Get old handle
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;
            uint64_t oldHandle = rResp.result().handle();

            // Reset
            octaneapi::ApiProjectManager::resetProjectRequest resetReq;
            resetReq.set_suppressui(true);
            octaneapi::ApiProjectManager::resetProjectResponse resetResp;
            auto ctx2 = makeContext(timeoutMs);
            s = projStub->resetProject(ctx2.get(), resetReq, &resetResp);
            if (!s.ok()) return s;

            // Use old handle — might work if SDK recycles the same ID, or NOT_FOUND
            octaneapi::ApiItem::nameRequest nReq;
            *nReq.mutable_objectptr() = makeRef(oldHandle);
            octaneapi::ApiItem::nameResponse nResp;
            auto ctx3 = makeContext(timeoutMs);
            return itemStub->name(ctx3.get(), nReq, &nResp);
        }
    });

    // Double destroy
    cat.tests.push_back({"double_destroy", "destroy valid node twice → second NOT_FOUND",
        grpc::StatusCode::NOT_FOUND,
        [projStub, nodeStub,
         itemStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            octaneapi::ApiNode::createRequest cReq;
            cReq.set_type(static_cast<octaneapi::NodeType>(89));
            *cReq.mutable_ownergraph() = makeRef(rResp.result().handle(),
                octaneapi::ObjectRef::ApiNodeGraph);
            cReq.set_configurepins(false);
            octaneapi::ApiNode::createResponse cResp;
            auto ctx2 = makeContext(timeoutMs);
            s = nodeStub->create(ctx2.get(), cReq, &cResp);
            if (!s.ok()) return s;
            uint64_t h = cResp.result().handle();

            // First destroy
            octaneapi::ApiItem::destroyRequest dReq;
            *dReq.mutable_objectptr() = makeRef(h);
            google::protobuf::Empty dResp;
            auto ctx3 = makeContext(timeoutMs);
            itemStub->destroy(ctx3.get(), dReq, &dResp);

            // Second destroy
            auto ctx4 = makeContext(timeoutMs);
            return itemStub->destroy(ctx4.get(), dReq, &dResp);
        }
    });

    // isRenderingPaused when not rendering
    cat.tests.push_back({"isRenderingPaused_idle",
        "isRenderingPaused when not rendering",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::isRenderingPausedRequest req;
            octaneapi::ApiRenderEngine::isRenderingPausedResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->isRenderingPaused(ctx.get(), req, &resp);
        }
    });

    // isCompiling when idle
    cat.tests.push_back({"isCompiling_idle", "isCompiling when idle",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::isCompilingRequest req;
            octaneapi::ApiRenderEngine::isCompilingResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->isCompiling(ctx.get(), req, &resp);
        }
    });

    // setRenderTargetNode with handle=0 (clears RT)
    cat.tests.push_back({"setRT_null", "setRenderTargetNode(0) — clears RT",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::setRenderTargetNodeRequest req;
            auto* ref = req.mutable_targetnode();
            ref->set_handle(0);
            octaneapi::ApiRenderEngine::setRenderTargetNodeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->setRenderTargetNode(ctx.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
