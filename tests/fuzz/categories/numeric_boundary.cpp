#include "categories/numeric_boundary.h"
#include "util/fuzz_values.h"
#include "util/connection.h"

#include "apirender.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"
#include "apinodesystem_7.grpc.pb.h"
#include "apiprojectmanager.grpc.pb.h"

namespace fuzz {

static octaneapi::ObjectRef makeRef(uint64_t handle,
    octaneapi::ObjectRef::ObjectType type = octaneapi::ObjectRef::ApiItem) {
    octaneapi::ObjectRef ref;
    ref.set_handle(handle);
    ref.set_type(type);
    return ref;
}

TestCategory makeNumericBoundary(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "numeric_boundary";

    auto channel = getSharedChannel(addr);
    auto renderStub = std::shared_ptr<octaneapi::ApiRenderEngineService::Stub>(octaneapi::ApiRenderEngineService::NewStub(channel));
    auto nodeStub = std::shared_ptr<octaneapi::ApiNodeService::Stub>(octaneapi::ApiNodeService::NewStub(channel));
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(octaneapi::ApiItemService::NewStub(channel));
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(octaneapi::ApiProjectManagerService::NewStub(channel));

    // getDeviceName with out-of-bounds index
    std::vector<uint32_t> badIndices = {999, UINT32_MAX, UINT32_MAX - 1};
    for (auto idx : badIndices) {
        cat.tests.push_back({"getDeviceName_idx_" + std::to_string(idx),
            "getDeviceName with index=" + std::to_string(idx),
            grpc::StatusCode::INVALID_ARGUMENT,
            [renderStub, idx, timeoutMs]() {
                octaneapi::ApiRenderEngine::getDeviceNameRequest req;
                req.set_index(idx);
                octaneapi::ApiRenderEngine::getDeviceNameResponse resp;
                auto ctx = makeContext(timeoutMs);
                return renderStub->getDeviceName(ctx.get(), req, &resp);
            }
        });
    }

    // getDeviceName index=0 should succeed (at least one GPU)
    cat.tests.push_back({"getDeviceName_idx_0", "getDeviceName with valid index=0",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::getDeviceNameRequest req;
            req.set_index(0);
            octaneapi::ApiRenderEngine::getDeviceNameResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->getDeviceName(ctx.get(), req, &resp);
        }
    });

    // connectedNodeIx with out-of-bounds pin index on a real node
    cat.tests.push_back({"connectedNodeIx_oob",
        "connectedNodeIx with pinIx=UINT32_MAX on root graph",
        grpc::StatusCode::INVALID_ARGUMENT,
        [projStub, nodeStub,
         itemStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;
            uint64_t graphHandle = rResp.result().handle();

            // Create a node to have valid pin access
            octaneapi::ApiNode::createRequest cReq;
            cReq.set_type(static_cast<octaneapi::NodeType>(89)); // NT_GEO_OBJECT
            *cReq.mutable_ownergraph() = makeRef(graphHandle, octaneapi::ObjectRef::ApiNodeGraph);
            cReq.set_configurepins(false);
            octaneapi::ApiNode::createResponse cResp;
            auto ctx2 = makeContext(timeoutMs);
            s = nodeStub->create(ctx2.get(), cReq, &cResp);
            if (!s.ok()) return s;
            uint64_t nodeHandle = cResp.result().handle();

            // Try out-of-bounds pin
            octaneapi::ApiNode::connectedNodeIxRequest req;
            *req.mutable_objectptr() = makeRef(nodeHandle, octaneapi::ObjectRef::ApiNode);
            req.set_pinix(UINT32_MAX);
            req.set_enterwrappernode(false);
            octaneapi::ApiNode::connectedNodeIxResponse resp;
            auto ctx3 = makeContext(timeoutMs);
            auto result = nodeStub->connectedNodeIx(ctx3.get(), req, &resp);

            // Cleanup
            octaneapi::ApiItem::destroyRequest dReq;
            *dReq.mutable_objectptr() = makeRef(nodeHandle);
            google::protobuf::Empty dResp;
            auto ctx4 = makeContext(timeoutMs);
            itemStub->destroy(ctx4.get(), dReq, &dResp);

            return result;
        }
    });

    // attrIdIx with out-of-bounds index
    cat.tests.push_back({"attrIdIx_oob",
        "attrIdIx with index=UINT32_MAX",
        grpc::StatusCode::INVALID_ARGUMENT,
        [projStub, itemStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            octaneapi::ApiItem::attrIdIxRequest req;
            *req.mutable_objectptr() = makeRef(rResp.result().handle());
            req.set_index(UINT32_MAX);
            octaneapi::ApiItem::attrIdIxResponse resp;
            auto ctx2 = makeContext(timeoutMs);
            return itemStub->attrIdIx(ctx2.get(), req, &resp);
        }
    });

    // connectToIx with out-of-bounds pin — server returns FAILED_PRECONDITION
    cat.tests.push_back({"connectToIx_oob",
        "connectToIx with pinIdx=999",
        grpc::StatusCode::FAILED_PRECONDITION,
        [projStub, nodeStub,
         itemStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            // Create a node
            octaneapi::ApiNode::createRequest cReq;
            cReq.set_type(static_cast<octaneapi::NodeType>(89));
            *cReq.mutable_ownergraph() = makeRef(rResp.result().handle(),
                octaneapi::ObjectRef::ApiNodeGraph);
            cReq.set_configurepins(false);
            octaneapi::ApiNode::createResponse cResp;
            auto ctx2 = makeContext(timeoutMs);
            s = nodeStub->create(ctx2.get(), cReq, &cResp);
            if (!s.ok()) return s;
            uint64_t nodeHandle = cResp.result().handle();

            octaneapi::ApiNode::connectToIxRequest req;
            *req.mutable_objectptr() = makeRef(nodeHandle, octaneapi::ObjectRef::ApiNode);
            req.set_pinidx(999);
            *req.mutable_sourcenode() = makeRef(nodeHandle, octaneapi::ObjectRef::ApiNode);
            req.set_evaluate(false);
            req.set_docyclecheck(false);
            google::protobuf::Empty resp;
            auto ctx3 = makeContext(timeoutMs);
            auto result = nodeStub->connectToIx(ctx3.get(), req, &resp);

            // Cleanup
            octaneapi::ApiItem::destroyRequest dReq;
            *dReq.mutable_objectptr() = makeRef(nodeHandle);
            google::protobuf::Empty dResp;
            auto ctx4 = makeContext(timeoutMs);
            itemStub->destroy(ctx4.get(), dReq, &dResp);

            return result;
        }
    });

    // getDeviceCount — should always succeed
    cat.tests.push_back({"getDeviceCount", "getDeviceCount basic call",
        grpc::StatusCode::OK,
        [renderStub, timeoutMs]() {
            octaneapi::ApiRenderEngine::getDeviceCountRequest req;
            octaneapi::ApiRenderEngine::getDeviceCountResponse resp;
            auto ctx = makeContext(timeoutMs);
            return renderStub->getDeviceCount(ctx.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
