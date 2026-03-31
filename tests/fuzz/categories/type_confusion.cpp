#include "categories/type_confusion.h"
#include "util/connection.h"

#include "apinodesystem_3.grpc.pb.h"
#include "apinodesystem_1.grpc.pb.h"
#include "apinodesystem_6.grpc.pb.h"
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

TestCategory makeTypeConfusion(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "type_confusion";

    auto channel = getSharedChannel(addr);
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(octaneapi::ApiProjectManagerService::NewStub(channel));
    auto nodeStub = std::shared_ptr<octaneapi::ApiNodeService::Stub>(octaneapi::ApiNodeService::NewStub(channel));
    auto graphStub = std::shared_ptr<octaneapi::ApiNodeGraphService::Stub>(octaneapi::ApiNodeGraphService::NewStub(channel));
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(octaneapi::ApiItemService::NewStub(channel));
    auto arrayStub = std::shared_ptr<octaneapi::ApiItemArrayService::Stub>(octaneapi::ApiItemArrayService::NewStub(channel));

    // Get root graph handle first
    cat.tests.push_back({"graph_as_node_pinCount", "graph handle in pinCount (type confusion)",
        grpc::StatusCode::INVALID_ARGUMENT,
        [projStub, nodeStub, timeoutMs]() {
            // Get root graph
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;
            uint64_t graphHandle = rResp.result().handle();

            // Use graph handle in node-only RPC
            octaneapi::ApiNode::pinCountRequest req;
            *req.mutable_objectptr() = makeRef(graphHandle, octaneapi::ObjectRef::ApiNodeGraph);
            octaneapi::ApiNode::pinCountResponse resp;
            auto ctx2 = makeContext(timeoutMs);
            return nodeStub->pinCount(ctx2.get(), req, &resp);
        }
    });

    // Node handle in array size (separate registry)
    cat.tests.push_back({"node_as_array", "node handle in array.size (wrong registry)",
        grpc::StatusCode::NOT_FOUND,
        [projStub, arrayStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            octaneapi::ApiItemArray::sizeRequest req;
            *req.mutable_objectptr() = makeRef(rResp.result().handle());
            octaneapi::ApiItemArray::sizeResponse resp;
            auto ctx2 = makeContext(timeoutMs);
            return arrayStub->size(ctx2.get(), req, &resp);
        }
    });

    // Node handle in getOwnedItems (graph-only method)
    cat.tests.push_back({"node_as_graph_getOwnedItems",
        "item handle in graph-only getOwnedItems",
        grpc::StatusCode::INVALID_ARGUMENT,
        [projStub, graphStub,
         nodeStub, itemStub, timeoutMs]() {
            // Get root graph, create a node, then use node handle as graph
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;
            uint64_t graphHandle = rResp.result().handle();

            // Create a simple node (NT_GEO_OBJECT = 89)
            octaneapi::ApiNode::createRequest cReq;
            cReq.set_type(static_cast<octaneapi::NodeType>(89));
            *cReq.mutable_ownergraph() = makeRef(graphHandle, octaneapi::ObjectRef::ApiNodeGraph);
            cReq.set_configurepins(false);
            octaneapi::ApiNode::createResponse cResp;
            auto ctx2 = makeContext(timeoutMs);
            s = nodeStub->create(ctx2.get(), cReq, &cResp);
            if (!s.ok()) return s;
            uint64_t nodeHandle = cResp.result().handle();

            // Use node handle in graph method
            octaneapi::ApiNodeGraph::getOwnedItemsRequest oReq;
            *oReq.mutable_objectptr() = makeRef(nodeHandle, octaneapi::ObjectRef::ApiNode);
            octaneapi::ApiNodeGraph::getOwnedItemsResponse oResp;
            auto ctx3 = makeContext(timeoutMs);
            auto result = graphStub->getOwnedItems(ctx3.get(), oReq, &oResp);

            // Cleanup: destroy the node
            octaneapi::ApiItem::destroyRequest dReq;
            *dReq.mutable_objectptr() = makeRef(nodeHandle);
            google::protobuf::Empty dResp;
            auto ctx4 = makeContext(timeoutMs);
            itemStub->destroy(ctx4.get(), dReq, &dResp);

            return result;
        }
    });

    // Synthetic array-range handle in item lookup
    cat.tests.push_back({"array_range_as_item",
        "handle in array range (2^52+1) in item lookup",
        grpc::StatusCode::NOT_FOUND,
        [itemStub, timeoutMs]() {
            octaneapi::ApiItem::isNodeRequest req;
            *req.mutable_objectptr() = makeRef((1ULL << 52) + 1);
            octaneapi::ApiItem::isNodeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->isNode(ctx.get(), req, &resp);
        }
    });

    // Wrong ObjectType enum — should still work (server reads handle, not type field)
    cat.tests.push_back({"wrong_objecttype_enum",
        "valid handle with wrong ObjectRef.type — server ignores type field",
        grpc::StatusCode::OK,
        [projStub, itemStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            octaneapi::ApiItem::isNodeRequest req;
            auto* ref = req.mutable_objectptr();
            ref->set_handle(rResp.result().handle());
            ref->set_type(static_cast<octaneapi::ObjectRef::ObjectType>(99)); // bogus type
            octaneapi::ApiItem::isNodeResponse resp;
            auto ctx2 = makeContext(timeoutMs);
            return itemStub->isNode(ctx2.get(), req, &resp);
        }
    });

    // Encoded pin-info handle as regular item
    cat.tests.push_back({"pininfo_handle_as_item",
        "encoded pin-info handle (high48|low16) in regular item lookup",
        grpc::StatusCode::NOT_FOUND,
        [itemStub, timeoutMs]() {
            // Encode: nodeHandle=42 << 16 | pinIndex=5
            uint64_t fake = (42ULL << 16) | 5;
            octaneapi::ApiItem::nameRequest req;
            *req.mutable_objectptr() = makeRef(fake);
            octaneapi::ApiItem::nameResponse resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->name(ctx.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
