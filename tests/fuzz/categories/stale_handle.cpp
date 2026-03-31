#include "categories/stale_handle.h"
#include "util/connection.h"

#include "apinodesystem_3.grpc.pb.h"
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

// Create a node, destroy it, return the now-stale handle
static uint64_t createAndDestroy(
    octaneapi::ApiNodeService::Stub* nodeStub,
    octaneapi::ApiItemService::Stub* itemStub,
    octaneapi::ApiProjectManagerService::Stub* projStub,
    int timeoutMs) {
    octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
    octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
    auto ctx1 = makeContext(timeoutMs);
    projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
    uint64_t graphHandle = rResp.result().handle();

    octaneapi::ApiNode::createRequest cReq;
    cReq.set_type(static_cast<octaneapi::NodeType>(89));
    *cReq.mutable_ownergraph() = makeRef(graphHandle, octaneapi::ObjectRef::ApiNodeGraph);
    cReq.set_configurepins(false);
    octaneapi::ApiNode::createResponse cResp;
    auto ctx2 = makeContext(timeoutMs);
    nodeStub->create(ctx2.get(), cReq, &cResp);
    uint64_t handle = cResp.result().handle();

    octaneapi::ApiItem::destroyRequest dReq;
    *dReq.mutable_objectptr() = makeRef(handle);
    google::protobuf::Empty dResp;
    auto ctx3 = makeContext(timeoutMs);
    itemStub->destroy(ctx3.get(), dReq, &dResp);

    return handle;
}

TestCategory makeStaleHandle(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "stale_handle";

    auto channel = getSharedChannel(addr);
    auto nodeStub = std::shared_ptr<octaneapi::ApiNodeService::Stub>(
        octaneapi::ApiNodeService::NewStub(channel));
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(
        octaneapi::ApiItemService::NewStub(channel));
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(
        octaneapi::ApiProjectManagerService::NewStub(channel));
    auto graphStub = std::shared_ptr<octaneapi::ApiNodeGraphService::Stub>(
        octaneapi::ApiNodeGraphService::NewStub(channel));

    // Each test: create node, destroy, try stale handle in specific RPC
    // All captured by value (shared_ptr copies)

    cat.tests.push_back({"stale_isNode", "stale handle in isNode",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiItem::isNodeRequest req;
            *req.mutable_objectptr() = makeRef(h);
            octaneapi::ApiItem::isNodeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->isNode(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_name", "stale handle in name",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiItem::nameRequest req;
            *req.mutable_objectptr() = makeRef(h);
            octaneapi::ApiItem::nameResponse resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->name(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_setName", "stale handle in setName",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiItem::setNameRequest req;
            *req.mutable_objectptr() = makeRef(h);
            req.set_name("test");
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->setName(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_outType", "stale handle in outType",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiItem::outTypeRequest req;
            *req.mutable_objectptr() = makeRef(h);
            octaneapi::ApiItem::outTypeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->outType(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_evaluate", "stale handle in evaluate",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiItem::evaluateRequest req;
            *req.mutable_objectptr() = makeRef(h);
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->evaluate(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_attrCount", "stale handle in attrCount",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiItem::attrCountRequest req;
            *req.mutable_objectptr() = makeRef(h);
            octaneapi::ApiItem::attrCountResponse resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->attrCount(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_destroy_again", "double destroy — second returns NOT_FOUND",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiItem::destroyRequest req;
            *req.mutable_objectptr() = makeRef(h);
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->destroy(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_pinCount", "stale handle in pinCount",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiNode::pinCountRequest req;
            *req.mutable_objectptr() = makeRef(h, octaneapi::ObjectRef::ApiNode);
            octaneapi::ApiNode::pinCountResponse resp;
            auto ctx = makeContext(timeoutMs);
            return nodeStub->pinCount(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_type", "stale handle in type",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiNode::typeRequest req;
            *req.mutable_objectptr() = makeRef(h, octaneapi::ObjectRef::ApiNode);
            octaneapi::ApiNode::typeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return nodeStub->type(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_connectedNodeIx", "stale handle in connectedNodeIx",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiNode::connectedNodeIxRequest req;
            *req.mutable_objectptr() = makeRef(h, octaneapi::ObjectRef::ApiNode);
            req.set_pinix(0);
            req.set_enterwrappernode(false);
            octaneapi::ApiNode::connectedNodeIxResponse resp;
            auto ctx = makeContext(timeoutMs);
            return nodeStub->connectedNodeIx(ctx.get(), req, &resp);
        }
    });

    cat.tests.push_back({"stale_getOwnedItems", "stale handle in getOwnedItems (as graph)",
        grpc::StatusCode::NOT_FOUND,
        [nodeStub, itemStub, projStub, graphStub, timeoutMs]() {
            uint64_t h = createAndDestroy(nodeStub.get(), itemStub.get(), projStub.get(), timeoutMs);
            octaneapi::ApiNodeGraph::getOwnedItemsRequest req;
            *req.mutable_objectptr() = makeRef(h, octaneapi::ObjectRef::ApiNodeGraph);
            octaneapi::ApiNodeGraph::getOwnedItemsResponse resp;
            auto ctx = makeContext(timeoutMs);
            return graphStub->getOwnedItems(ctx.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
