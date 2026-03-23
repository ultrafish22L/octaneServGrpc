// OctaneServGrpc Server — hosts all Octane gRPC services
// Serves the same Beta 2 protos that octaneWebR expects.
// Each service maps gRPC calls directly to Octane SDK C++ API calls.
//
// HARDENING: Every RPC is wrapped in GRPC_SAFE which catches C++ exceptions
// and (on Windows) SEH exceptions. The server NEVER crashes from bad input.
// Every error returns a descriptive gRPC Status with full context.

#include "grpc_server.h"
#include "sdk_engine.h"
#include "util/handle_registry.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_interceptor.h>
#include <iostream>
#include <mutex>
#include <atomic>
#include <thread>
#include <sstream>
#include <string>

#include "util/server_log.h"
#include "util/logging_interceptor.h"
#include "util/callback_dispatcher.h"

// Octane SDK
#include "octaneapi.h"

// Generated proto service headers
#include "apiinfo.grpc.pb.h"
#include "apiprojectmanager.grpc.pb.h"
#include "apichangemanager.grpc.pb.h"
#include "apirender.grpc.pb.h"
#include "livelink.grpc.pb.h"
#include "callbackstream.grpc.pb.h"
#include "apinodesystem_1.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"
#include "apinodesystem_6.grpc.pb.h"
#include "apinodesystem_7.grpc.pb.h"

namespace OctaneServ {

// Shared handle registry — all services access the same instance
static HandleRegistry* sHandleRegistry = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
// Hardening infrastructure
// ═══════════════════════════════════════════════════════════════════════════

// GRPC_SAFE: Wraps every RPC method body. Catches C++ exceptions and
// (on Windows) SEH exceptions. Returns descriptive gRPC error, never crashes.
//
// Usage:
//   grpc::Status myMethod(...) override {
//       GRPC_SAFE("MyService", {
//           // ... method body ...
//           return grpc::Status::OK;
//       });
//   }

// GRPC_SAFE_BEGIN / GRPC_SAFE_END: Wraps every RPC method body.
// Catches all C++ exceptions and returns a descriptive gRPC error.
// The server NEVER crashes from a bad request.
//
// NOTE on SEH (Windows Structured Exception Handling):
// __try/__except cannot coexist with C++ objects that have destructors
// (grpc::Status, std::string) in the same function — MSVC error C2712.
// Instead, we enable /EHa in CMakeLists.txt which makes catch(...)
// also catch SEH exceptions (access violations, etc). This gives us
// the same protection without the __try/__except syntax limitation.
//
// Usage:
//   grpc::Status myMethod(...) override {
//       GRPC_SAFE_BEGIN(SVC)
//           // ... method body ...
//           return grpc::Status::OK;
//       GRPC_SAFE_END(SVC)
//   }

#define GRPC_SAFE_BEGIN(svc) \
    try {

#define GRPC_SAFE_END(svc) \
    } catch (const std::exception& _e) { \
        std::string _msg = std::string(svc) + "." + __func__ + ": " + _e.what(); \
        ServerLog::instance().err(svc, __func__, _msg); \
        return grpc::Status(grpc::StatusCode::INTERNAL, _msg); \
    } catch (...) { \
        std::string _msg = std::string(svc) + "." + __func__ + ": unknown C++ exception"; \
        ServerLog::instance().err(svc, __func__, _msg); \
        return grpc::Status(grpc::StatusCode::INTERNAL, _msg); \
    }

// ── Validated lookup helpers ─────────────────────────────────────────────
// Every handle lookup goes through these. Returns descriptive NOT_FOUND/
// INVALID_ARGUMENT errors instead of silently returning null.

static grpc::Status failNotFound(const char* svc, const char* method,
    uint64_t handle, const char* what = "item")
{
    std::ostringstream oss;
    oss << svc << "." << method << ": " << what << " handle " << handle
        << " not found in registry (stale or never registered)";
    return grpc::Status(grpc::StatusCode::NOT_FOUND, oss.str());
}

static grpc::Status failInvalidArg(const char* svc, const char* method,
    const std::string& detail)
{
    std::string msg = std::string(svc) + "." + method + ": " + detail;
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, msg);
}

static grpc::Status failPrecondition(const char* svc, const char* method,
    const std::string& detail)
{
    std::string msg = std::string(svc) + "." + method + ": " + detail;
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, msg);
}

// Lookup item by handle, return error status if not found or null handle.
static Octane::ApiItem* requireItem(uint64_t handle, const char* svc,
    const char* method, grpc::Status& status)
{
    if (handle == 0) {
        status = failInvalidArg(svc, method, "handle is 0 (must be non-zero uniqueId)");
        return nullptr;
    }
    auto* item = sHandleRegistry->Lookup(handle);
    if (!item) {
        status = failNotFound(svc, method, handle);
        return nullptr;
    }
    return item;
}

// Lookup item and downcast to graph, return error if not found or not a graph.
static Octane::ApiNodeGraph* requireGraph(uint64_t handle, const char* svc,
    const char* method, grpc::Status& status)
{
    auto* item = requireItem(handle, svc, method, status);
    if (!item) return nullptr;
    auto* graph = item->toGraph();
    if (!graph) {
        std::ostringstream oss;
        oss << "item " << handle << " is not a graph (cannot call " << method << " on a "
            << (item->isNode() ? "node" : "non-graph item") << ")";
        status = failInvalidArg(svc, method, oss.str());
        return nullptr;
    }
    return graph;
}

// Lookup item and downcast to node, return error if not found or not a node.
static Octane::ApiNode* requireNode(uint64_t handle, const char* svc,
    const char* method, grpc::Status& status)
{
    auto* item = requireItem(handle, svc, method, status);
    if (!item) return nullptr;
    auto* node = item->toNode();
    if (!node) {
        std::ostringstream oss;
        oss << "item " << handle << " is not a node (cannot call " << method << " on a "
            << (item->isGraph() ? "graph" : "non-node item") << ")";
        status = failInvalidArg(svc, method, oss.str());
        return nullptr;
    }
    return node;
}

// Lookup array by handle.
static Octane::ApiItemArray* requireArray(uint64_t handle, const char* svc,
    const char* method, grpc::Status& status)
{
    if (handle == 0) {
        status = failInvalidArg(svc, method, "array handle is 0");
        return nullptr;
    }
    auto* arr = sHandleRegistry->LookupArray(handle);
    if (!arr) {
        status = failNotFound(svc, method, handle, "array");
        return nullptr;
    }
    return arr;
}

// ═══════════════════════════════════════════════════════════════════════════
// ApiInfoService
// ═══════════════════════════════════════════════════════════════════════════
class InfoServiceImpl final : public octaneapi::ApiInfoService::Service {
    static constexpr const char* SVC = "ApiInfoService";
public:
    grpc::Status octaneVersion(grpc::ServerContext*, const octaneapi::ApiInfo::octaneVersionRequest*,
        octaneapi::ApiInfo::octaneVersionResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiInfo::octaneVersion());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status octaneName(grpc::ServerContext*, const octaneapi::ApiInfo::octaneNameRequest*,
        octaneapi::ApiInfo::octaneNameResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            const char* n = Octane::ApiInfo::octaneName();
            response->set_result(n ? n : "");
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status isDemoVersion(grpc::ServerContext*, const octaneapi::ApiInfo::isDemoVersionRequest*,
        octaneapi::ApiInfo::isDemoVersionResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiInfo::isDemoVersion());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status isSubscriptionVersion(grpc::ServerContext*, const octaneapi::ApiInfo::isSubscriptionVersionRequest*,
        octaneapi::ApiInfo::isSubscriptionVersionResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiInfo::isSubscriptionVersion());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status tierIdx(grpc::ServerContext*, const octaneapi::ApiInfo::tierIdxRequest*,
        octaneapi::ApiInfo::tierIdxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiInfo::tierIdx());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Node type metadata ────────────────────────────────────────────
    grpc::Status nodeInfo(grpc::ServerContext*, const octaneapi::ApiInfo::nodeInfoRequest* request,
        octaneapi::ApiInfo::nodeInfoResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::NodeType type = static_cast<Octane::NodeType>(request->type());
            const Octane::ApiNodeInfo* info = Octane::ApiInfo::nodeInfo(type);
            if (!info) {
                std::ostringstream oss;
                oss << "no nodeInfo for type " << static_cast<int>(type);
                return failNotFound(SVC, __func__, static_cast<uint64_t>(type), "nodeType");
            }
            auto* r = response->mutable_result();
            r->set_type(static_cast<octaneapi::NodeType>(info->mType));
            r->set_description(info->mDescription ? info->mDescription : "");
            r->set_outtype(static_cast<octaneapi::NodePinType>(info->mOutType));
            r->set_nodecolor(info->mNodeColor);
            r->set_islinker(info->mIsLinker);
            r->set_isoutputlinker(info->mIsOutputLinker);
            r->set_takespindefaultvalue(info->mTakesPinDefaultValue);
            r->set_ishidden(info->mIsHidden);
            r->set_iscreatablebyapi(info->mIsCreatableByApi);
            r->set_isscriptgraphwrapper(info->mIsScriptGraphWrapper);
            r->set_istypedtexturenode(info->mIsTypedTextureNode);
            r->set_category(info->mCategory ? info->mCategory : "");
            r->set_defaultname(info->mDefaultName ? info->mDefaultName : "");
            r->set_attributeinfocount(info->mAttributeInfoCount);
            r->set_pininfocount(info->mPinInfoCount);
            r->set_movableinputcountattribute(static_cast<octaneapi::AttributeId>(info->mMovableInputCountAttribute));
            r->set_movableinputpincount(info->mMovableInputPinCount);
            r->set_movableinputformat(static_cast<octaneapi::MovableInputFormat>(info->mMovableInputFormat));
            r->set_movableinputname(info->mMovableInputName ? info->mMovableInputName : "");
            r->set_minversion(info->mMinVersion);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Pin metadata (returns ApiNodePinInfo as packed proto, not opaque handle) ──
    grpc::Status nodePinInfo(grpc::ServerContext*, const octaneapi::ApiInfo::nodePinInfoRequest* request,
        octaneapi::ApiInfo::nodePinInfoResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::NodeType nodeType = static_cast<Octane::NodeType>(request->nodetype());
            uint32_t pinIx = request->pinix();
            const Octane::ApiNodePinInfo* info = Octane::ApiInfo::nodePinInfo(nodeType, pinIx);
            if (!info) {
                std::ostringstream oss;
                oss << "no nodePinInfo for type " << static_cast<int>(nodeType) << " pin " << pinIx;
                return failNotFound(SVC, __func__, static_cast<uint64_t>(nodeType), "pinInfo");
            }
            // The proto returns ObjectRef — old gRPC pattern. Return 0 handle.
            // The MCP caches the raw gRPC response; actual pin data comes from
            // ApiNode::pinNameIx/pinTypeIx which are now implemented.
            response->mutable_result()->set_handle(0);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Attribute metadata (static, by type + attrId) ─────────────────
    grpc::Status attributeInfo(grpc::ServerContext*, const octaneapi::ApiInfo::attributeInfoRequest* request,
        octaneapi::ApiInfo::attributeInfoResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::NodeType type = static_cast<Octane::NodeType>(request->type());
            Octane::AttributeId attrId = static_cast<Octane::AttributeId>(request->attrid());
            const Octane::ApiAttributeInfo* info = Octane::ApiInfo::attributeInfo(type, attrId);
            if (!info) {
                std::ostringstream oss;
                oss << "no attributeInfo for type " << static_cast<int>(type) << " attr " << static_cast<int>(attrId);
                return failNotFound(SVC, __func__, static_cast<uint64_t>(attrId), "attributeInfo");
            }
            auto* r = response->mutable_result();
            r->set_id(static_cast<octaneapi::AttributeId>(info->mId));
            r->set_type(static_cast<octaneapi::AttributeType>(info->mType));
            r->set_isarray(info->mIsArray);
            r->set_filenameattribute(static_cast<octaneapi::AttributeId>(info->mFileNameAttribute));
            r->set_packageattribute(static_cast<octaneapi::AttributeId>(info->mPackageAttribute));
            r->set_description(info->mDescription ? info->mDescription : "");
            auto* di = r->mutable_defaultints();
            di->set_x(info->mDefaultInts.x); di->set_y(info->mDefaultInts.y);
            di->set_z(info->mDefaultInts.z); di->set_w(info->mDefaultInts.w);
            auto* dl = r->mutable_defaultlongs();
            dl->set_x(info->mDefaultLongs.x); dl->set_y(info->mDefaultLongs.y);
            auto* df = r->mutable_defaultfloats();
            df->set_x(info->mDefaultFloats.x); df->set_y(info->mDefaultFloats.y);
            df->set_z(info->mDefaultFloats.z); df->set_w(info->mDefaultFloats.w);
            r->set_defaultstring(info->mDefaultString ? info->mDefaultString : "");
            r->set_minversion(info->mMinVersion);
            r->set_endversion(info->mEndVersion);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Compatible types for a pin type ───────────────────────────────
    grpc::Status getCompatibleTypes(grpc::ServerContext*, const octaneapi::ApiInfo::getCompatibleTypesRequest* request,
        octaneapi::ApiInfo::getCompatibleTypesResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::NodePinType outType = static_cast<Octane::NodePinType>(request->outtype());
            Octane::NodeGraphType* compatGraphs = nullptr;
            size_t compatGraphsSize = 0;
            Octane::NodeType* compatNodes = nullptr;
            size_t compatNodesSize = 0;
            Octane::ApiInfo::getCompatibleTypes(outType, compatGraphs, compatGraphsSize, compatNodes, compatNodesSize);

            auto* cg = response->mutable_compatgraphs();
            for (size_t i = 0; i < compatGraphsSize; ++i) {
                cg->add_data(static_cast<octaneapi::NodeGraphType>(compatGraphs[i]));
            }
            response->set_compatgraphssize(static_cast<uint32_t>(compatGraphsSize));

            auto* cn = response->mutable_compatnodes();
            for (size_t i = 0; i < compatNodesSize; ++i) {
                cn->add_data(static_cast<octaneapi::NodeType>(compatNodes[i]));
            }
            response->set_compatnodessize(static_cast<uint32_t>(compatNodesSize));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiProjectManagerService
// ═══════════════════════════════════════════════════════════════════════════
class ProjectManagerServiceImpl final : public octaneapi::ApiProjectManagerService::Service {
    static constexpr const char* SVC = "ApiProjectManagerService";
public:
    grpc::Status rootNodeGraph(grpc::ServerContext*, const octaneapi::ApiProjectManager::rootNodeGraphRequest*,
        octaneapi::ApiProjectManager::rootNodeGraphResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiRootNodeGraph& root = Octane::ApiProjectManager::rootNodeGraph();
            uint64_t handle = sHandleRegistry->Register(reinterpret_cast<Octane::ApiItem*>(&root));
            response->mutable_result()->set_handle(handle);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status resetProject(grpc::ServerContext*, const octaneapi::ApiProjectManager::resetProjectRequest*,
        octaneapi::ApiProjectManager::resetProjectResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            bool result = Octane::ApiProjectManager::resetProject();
            sHandleRegistry->Clear();
            response->set_result(result);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status loadProject(grpc::ServerContext*, const octaneapi::ApiProjectManager::loadProjectRequest* request,
        octaneapi::ApiProjectManager::loadProjectResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            std::string path = request->projectpath();
            if (path.empty()) {
                return failInvalidArg(SVC, __func__, "projectpath is empty");
            }
            bool evaluate = request->evaluate();
            bool result = Octane::ApiProjectManager::loadProject(path.c_str(), nullptr, 0, evaluate);
            sHandleRegistry->Clear();
            response->set_result(result);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status saveProject(grpc::ServerContext*, const octaneapi::ApiProjectManager::saveProjectRequest*,
        octaneapi::ApiProjectManager::saveProjectResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            bool result = Octane::ApiProjectManager::saveProject();
            response->set_result(result);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status saveProjectAs(grpc::ServerContext*, const octaneapi::ApiProjectManager::saveProjectAsRequest* request,
        octaneapi::ApiProjectManager::saveProjectAsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            std::string path = request->path();
            if (path.empty()) {
                return failInvalidArg(SVC, __func__, "path is empty");
            }
            bool result = Octane::ApiProjectManager::saveProjectAs(path.c_str());
            response->set_result(result);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getCurrentProject(grpc::ServerContext*, const octaneapi::ApiProjectManager::getCurrentProjectRequest*,
        octaneapi::ApiProjectManager::getCurrentProjectResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            const char* path = Octane::ApiProjectManager::getCurrentProject();
            response->set_result(path ? path : "");
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiChangeManagerService
// ═══════════════════════════════════════════════════════════════════════════
class ChangeManagerServiceImpl final : public octaneapi::ApiChangeManagerService::Service {
    static constexpr const char* SVC = "ApiChangeManagerService";
public:
    grpc::Status update(grpc::ServerContext*, const octaneapi::ApiChangeManager::updateRequest*,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiChangeManager::update();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiRenderEngineService
// ═══════════════════════════════════════════════════════════════════════════
class RenderEngineServiceImpl final : public octaneapi::ApiRenderEngineService::Service {
    static constexpr const char* SVC = "ApiRenderEngineService";
public:
    grpc::Status getDeviceCount(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getDeviceCountRequest*,
        octaneapi::ApiRenderEngine::getDeviceCountResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiRenderEngine::getDeviceCount());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getDeviceName(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getDeviceNameRequest* request,
        octaneapi::ApiRenderEngine::getDeviceNameResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            uint32_t index = request->index();
            uint32_t count = Octane::ApiRenderEngine::getDeviceCount();
            if (index >= count) {
                std::ostringstream oss;
                oss << "device index " << index << " out of range (device count is " << count << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            const char* name = Octane::ApiRenderEngine::getDeviceName(index);
            response->set_result(name ? name : "");
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status clayMode(grpc::ServerContext*, const octaneapi::ApiRenderEngine::clayModeRequest*,
        octaneapi::ApiRenderEngine::clayModeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            auto mode = Octane::ApiRenderEngine::clayMode();
            response->set_result(static_cast<octaneapi::ClayMode>(mode));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setClayMode(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setClayModeRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            int mode = request->mode();
            if (mode < 0 || mode > 3) {
                std::ostringstream oss;
                oss << "clay mode value " << mode << " is not valid (valid range: 0-3)";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            Octane::ApiRenderEngine::setClayMode(static_cast<Octane::ClayMode>(mode));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getSubSampleMode(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getSubSampleModeRequest*,
        octaneapi::ApiRenderEngine::getSubSampleModeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            auto mode = Octane::ApiRenderEngine::getSubSampleMode();
            response->set_result(static_cast<octaneapi::SubSampleMode>(mode));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setSubSampleMode(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setSubSampleModeRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiRenderEngine::setSubSampleMode(static_cast<Octane::SubSampleMode>(request->mode()));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status renderPriority(grpc::ServerContext*, const octaneapi::ApiRenderEngine::renderPriorityRequest*,
        octaneapi::ApiRenderEngine::renderPriorityResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(static_cast<octaneapi::ApiRenderEngine_RenderPriority>(Octane::ApiRenderEngine::renderPriority()));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setRenderPriority(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setRenderPriorityRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            int prio = request->priority();
            if (prio < 0 || prio >= 3) {
                std::ostringstream oss;
                oss << "render priority " << prio << " is not valid (valid: 0=LOW, 1=MEDIUM, 2=HIGH)";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            Octane::ApiRenderEngine::setRenderPriority(static_cast<Octane::ApiRenderEngine::RenderPriority>(prio));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setRenderTargetNode(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setRenderTargetNodeRequest* request,
        octaneapi::ApiRenderEngine::setRenderTargetNodeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            uint64_t handle = request->targetnode().handle();
            // handle 0 means "clear render target" (set to null)
            if (handle == 0) {
                bool result = Octane::ApiRenderEngine::setRenderTargetNode(nullptr);
                response->set_result(result);
                return grpc::Status::OK;
            }
            grpc::Status status;
            auto* node = requireNode(handle, SVC, __func__, status);
            if (!node) return status;
            bool result = Octane::ApiRenderEngine::setRenderTargetNode(node);
            response->set_result(result);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getRenderTargetNode(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getRenderTargetNodeRequest*,
        octaneapi::ApiRenderEngine::getRenderTargetNodeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiNode* node = Octane::ApiRenderEngine::getRenderTargetNode();
            if (node) {
                uint64_t handle = sHandleRegistry->Register(node);
                response->mutable_result()->set_handle(handle);
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status stopRendering(grpc::ServerContext*, const octaneapi::ApiRenderEngine::stopRenderingRequest*,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiRenderEngine::stopRendering();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status continueRendering(grpc::ServerContext*, const octaneapi::ApiRenderEngine::continueRenderingRequest*,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiRenderEngine::continueRendering();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status restartRendering(grpc::ServerContext*, const octaneapi::ApiRenderEngine::restartRenderingRequest*,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiRenderEngine::restartRendering();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status pauseRendering(grpc::ServerContext*, const octaneapi::ApiRenderEngine::pauseRenderingRequest*,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiRenderEngine::pauseRendering();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status isRenderingPaused(grpc::ServerContext*, const octaneapi::ApiRenderEngine::isRenderingPausedRequest*,
        octaneapi::ApiRenderEngine::isRenderingPausedResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiRenderEngine::isRenderingPaused());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getMemoryUsage(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getMemoryUsageRequest* request,
        octaneapi::ApiRenderEngine::getMemoryUsageResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            uint32_t deviceIx = request->deviceix();
            uint32_t count = Octane::ApiRenderEngine::getDeviceCount();
            if (deviceIx >= count) {
                std::ostringstream oss;
                oss << "device index " << deviceIx << " out of range (device count is " << count << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            Octane::ApiDeviceMemoryUsage memUsage;
            Octane::ApiRenderEngine::getMemoryUsage(deviceIx, memUsage);
            auto* mu = response->mutable_memusage();
            mu->set_useddevicememory(memUsage.mUsedDeviceMemory);
            mu->set_freedevicememory(memUsage.mFreeDeviceMemory);
            mu->set_totaldevicememory(memUsage.mTotalDeviceMemory);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getSceneBounds(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getSceneBoundsRequest*,
        octaneapi::ApiRenderEngine::getSceneBoundsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::float_3 bmin, bmax;
            bool result = Octane::ApiRenderEngine::getSceneBounds(bmin, bmax);
            response->set_result(result);
            auto* pmin = response->mutable_bboxmin();
            pmin->set_x(bmin.x); pmin->set_y(bmin.y); pmin->set_z(bmin.z);
            auto* pmax = response->mutable_bboxmax();
            pmax->set_x(bmax.x); pmax->set_y(bmax.y); pmax->set_z(bmax.z);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getRealTime(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getRealTimeRequest*,
        octaneapi::ApiRenderEngine::getRealTimeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiRenderEngine::getRealTime());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setOnNewImageCallback(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setOnNewImageCallbackRequest*,
        octaneapi::ApiRenderEngine::setOnNewImageCallbackResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            // Callback registration is handled by StreamCallbackService
            response->set_callbackid(1);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setOnNewStatisticsCallback(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setOnNewStatisticsCallbackRequest*,
        octaneapi::ApiRenderEngine::setOnNewStatisticsCallbackResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_callbackid(2);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setOnRenderFailureCallback(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setOnRenderFailureCallbackRequest*,
        octaneapi::ApiRenderEngine::setOnRenderFailureCallbackResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_callbackid(3);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Render Statistics ─────────────────────────────────────────────
    grpc::Status getRenderStatistics(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getRenderStatisticsRequest*,
        octaneapi::ApiRenderEngine::getRenderStatisticsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::RenderResultStatistics stats;
            stats.clear();
            Octane::ApiRenderEngine::getRenderStatistics(stats);

            auto* s = response->mutable_statistics();
            auto* setSize = s->mutable_setsize();
            setSize->set_x(stats.mSetSize.x); setSize->set_y(stats.mSetSize.y);
            auto* usedSize = s->mutable_usedsize();
            usedSize->set_x(stats.mUsedSize.x); usedSize->set_y(stats.mUsedSize.y);
            s->set_subsamplemode(static_cast<octaneapi::SubSampleMode>(stats.mSubSampleMode));
            s->set_upsamplingratio(stats.mUpSamplingRatio);
            s->set_buffertype(static_cast<octaneapi::TonemapBufferType>(stats.mBufferType));
            s->set_colorspace(static_cast<octaneapi::NamedColorSpace>(stats.mColorSpace));
            s->set_islinear(stats.mIsLinear);
            s->set_hasalpha(stats.mHasAlpha);
            s->set_premultipliedalphatype(static_cast<octaneapi::PremultipliedAlphaType>(stats.mPremultipliedAlphaType));
            s->set_keepenvironment(stats.mKeepEnvironment);
            s->mutable_changelevel()->set_value(static_cast<uint64_t>(stats.mChangeLevel));
            s->set_haspendingupdates(stats.mHasPendingUpdates);
            s->set_deepbincount(stats.mDeepBinCount);
            s->set_deepseedspp(stats.mDeepSeedSpp);
            s->set_cryptomatteseedspp(stats.mCryptomatteSeedSpp);
            s->set_deeppassesenabled(stats.mDeepPassesEnabled);
            s->set_tonemappassescount(stats.mTonemapPassesCount);
            s->set_passescount(stats.mPassesCount);
            s->set_beautywipecount(stats.mBeautyWipeCount);
            s->set_beautysamplesperpixel(stats.mBeautySamplesPerPixel);
            s->set_beautymaxsamplesperpixel(stats.mBeautyMaxSamplesPerPixel);
            s->set_beautysamplespersecond(stats.mBeautySamplesPerSecond);
            s->set_regionsamplesperpixel(stats.mRegionSamplesPerPixel);
            s->set_denoisedsamplesperpixel(stats.mDenoisedSamplesPerPixel);
            s->set_regiondenoisedsamplesperpixel(stats.mRegionDenoisedSamplesPerPixel);
            s->set_infowipecount(stats.mInfoWipeCount);
            s->set_infosamplesperpixel(stats.mInfoSamplesPerPixel);
            s->set_infomaxsamplesperpixel(stats.mInfoMaxSamplesPerPixel);
            s->set_infosamplespersecond(stats.mInfoSamplesPerSecond);
            s->set_state(static_cast<octaneapi::RenderState>(stats.mState));
            s->set_rendertime(stats.mRenderTime);
            s->set_estimatedrendertime(stats.mEstimatedRenderTime);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Save Image ────────────────────────────────────────────────────
    grpc::Status saveImage1(grpc::ServerContext*, const octaneapi::ApiRenderEngine::saveImage1Request* request,
        octaneapi::ApiRenderEngine::saveImage1Response* response) override {
        GRPC_SAFE_BEGIN(SVC)
            if (request->fullpath().empty()) {
                return failInvalidArg(SVC, __func__, "fullPath is empty");
            }
            Octane::RenderPassId passId = static_cast<Octane::RenderPassId>(request->renderpassid());
            Octane::ImageSaveFormat fmt = static_cast<Octane::ImageSaveFormat>(request->imagesaveformat());
            Octane::NamedColorSpace cs = static_cast<Octane::NamedColorSpace>(request->colorspace());
            Octane::PremultipliedAlphaType pa = static_cast<Octane::PremultipliedAlphaType>(request->premultipliedalphatype());
            Octane::ExrCompressionType exrComp = static_cast<Octane::ExrCompressionType>(request->exrcompressiontype());
            float exrLevel = request->exrcompressionlevel();
            if (exrLevel <= 0.f) exrLevel = 45.f;
            bool result = Octane::ApiRenderEngine::saveImage(
                passId, request->fullpath().c_str(), fmt, cs, pa, exrComp, exrLevel, request->asynchronous());
            response->set_result(result);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Enabled AOVs ──────────────────────────────────────────────────
    grpc::Status getEnabledAovs(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getEnabledAovsRequest* request,
        octaneapi::ApiRenderEngine::getEnabledAovsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiNode* rtNode = nullptr;
            if (request->rendertargetnode().handle() != 0) {
                grpc::Status status;
                rtNode = requireNode(request->rendertargetnode().handle(), SVC, __func__, status);
                if (!rtNode) return status;
            } else {
                rtNode = Octane::ApiRenderEngine::getRenderTargetNode();
            }
            Octane::ApiArray<Octane::RenderPassId> aovIds;
            Octane::ApiRenderEngine::getEnabledAovs(rtNode, aovIds);
            auto* out = response->mutable_aovids();
            for (size_t i = 0; i < aovIds.mSize; ++i) {
                out->add_data(static_cast<octaneapi::RenderPassId>(aovIds.mData[i]));
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Geometry / Texture / Resource Statistics ──────────────────────
    grpc::Status getGeometryStatistics(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getGeometryStatisticsRequest*,
        octaneapi::ApiRenderEngine::getGeometryStatisticsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiGeometryStatistics gs;
            memset(&gs, 0, sizeof(gs));
            Octane::ApiRenderEngine::getGeometryStatistics(gs);
            auto* s = response->mutable_stats();
            s->set_tricount(gs.mTriCount);
            s->set_disptricount(gs.mDispTriCount);
            s->set_hairsegcount(gs.mHairSegCount);
            s->set_voxelcount(gs.mVoxelCount);
            s->set_gaussiansplatcount(gs.mGaussianSplatCount);
            s->set_spherecount(gs.mSphereCount);
            s->set_instancecount(gs.mInstanceCount);
            s->set_emitpricount(gs.mEmitPriCount);
            s->set_emitinstancecount(gs.mEmitInstanceCount);
            s->set_analyticlicount(gs.mAnalyticLiCount);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status getTexturesStatistics(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getTexturesStatisticsRequest*,
        octaneapi::ApiRenderEngine::getTexturesStatisticsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiTextureStatistics ts;
            memset(&ts, 0, sizeof(ts));
            Octane::ApiRenderEngine::getTexturesStatistics(ts);
            auto* s = response->mutable_texturestats();
            s->set_usedrgba32textures(ts.mUsedRgba32Textures);
            s->set_usedrgba64textures(ts.mUsedRgba64Textures);
            s->set_usedy8textures(ts.mUsedY8Textures);
            s->set_usedy16textures(ts.mUsedY16Textures);
            s->set_usedvirtualtextures(ts.mUsedVirtualTextures);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status getResourceStatistics(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getResourceStatisticsRequest* request,
        octaneapi::ApiRenderEngine::getResourceStatisticsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            uint32_t deviceIx = request->deviceix();
            uint32_t count = Octane::ApiRenderEngine::getDeviceCount();
            if (deviceIx >= count) {
                std::ostringstream oss;
                oss << "device index " << deviceIx << " out of range (device count is " << count << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            Octane::ApiDeviceResourceStatistics rs;
            memset(&rs, 0, sizeof(rs));
            Octane::ApiRenderEngine::getResourceStatistics(deviceIx,
                static_cast<Octane::MemoryLocation>(request->memorylocation()), rs);
            auto* s = response->mutable_resourcestats();
            s->set_runtimedatasize(rs.mRuntimeDataSize);
            s->set_filmdatasize(rs.mFilmDataSize);
            s->set_geometrydatasize(rs.mGeometryDataSize);
            s->set_nodesystemdatasize(rs.mNodeSystemDataSize);
            s->set_imagesdatasize(rs.mImagesDataSize);
            s->set_compositordatasize(rs.mCompositorDataSize);
            s->set_denoiserdatasize(rs.mDenoiserDataSize);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Render state booleans ─────────────────────────────────────────
    grpc::Status isCompiling(grpc::ServerContext*, const octaneapi::ApiRenderEngine::isCompilingRequest*,
        octaneapi::ApiRenderEngine::isCompilingResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiRenderEngine::isCompiling());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status isCompressingTextures(grpc::ServerContext*, const octaneapi::ApiRenderEngine::isCompressingTexturesRequest*,
        octaneapi::ApiRenderEngine::isCompressingTexturesResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiRenderEngine::isCompressingTextures());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status hasPendingRenderData(grpc::ServerContext*, const octaneapi::ApiRenderEngine::hasPendingRenderDataRequest*,
        octaneapi::ApiRenderEngine::hasPendingRenderDataResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiRenderEngine::hasPendingRenderData());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status isRenderFailure(grpc::ServerContext*, const octaneapi::ApiRenderEngine::isRenderFailureRequest*,
        octaneapi::ApiRenderEngine::isRenderFailureResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_result(Octane::ApiRenderEngine::isRenderFailure());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// LiveLinkService
// ═══════════════════════════════════════════════════════════════════════════
class LiveLinkServiceImpl final : public livelinkapi::LiveLinkService::Service {
    static constexpr const char* SVC = "LiveLinkService";
public:
    grpc::Status GetCamera(grpc::ServerContext*, const livelinkapi::Empty*,
        livelinkapi::CameraState* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiNode* rt = Octane::ApiRenderEngine::getRenderTargetNode();
            if (!rt) {
                setDefaultCamera(response);
                return grpc::Status::OK;
            }
            Octane::ApiNode* cam = rt->connectedNode(Octane::P_CAMERA, false);
            if (!cam) {
                setDefaultCamera(response);
                return grpc::Status::OK;
            }
            Octane::float_3 pos, target, up;
            cam->getPinValue(Octane::P_POSITION, pos);
            cam->getPinValue(Octane::P_TARGET, target);
            cam->getPinValue(Octane::P_UP, up);

            auto* rpos = response->mutable_position();
            rpos->set_x(pos.x); rpos->set_y(pos.y); rpos->set_z(pos.z);
            auto* rtgt = response->mutable_target();
            rtgt->set_x(target.x); rtgt->set_y(target.y); rtgt->set_z(target.z);
            auto* rup = response->mutable_up();
            rup->set_x(up.x); rup->set_y(up.y); rup->set_z(up.z);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status SetCamera(grpc::ServerContext*, const livelinkapi::CameraState* request,
        livelinkapi::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiNode* rt = Octane::ApiRenderEngine::getRenderTargetNode();
            if (!rt) return failPrecondition(SVC, __func__,
                "no render target node set (call setRenderTargetNode first)");
            Octane::ApiNode* cam = rt->connectedNode(Octane::P_CAMERA, false);
            if (!cam) return failPrecondition(SVC, __func__,
                "render target has no camera node connected to P_CAMERA pin");

            if (request->has_position()) {
                Octane::float_3 pos = {(float)request->position().x(), (float)request->position().y(), (float)request->position().z()};
                cam->setPinValue(Octane::P_POSITION, pos, false);
            }
            if (request->has_target()) {
                Octane::float_3 tgt = {(float)request->target().x(), (float)request->target().y(), (float)request->target().z()};
                cam->setPinValue(Octane::P_TARGET, tgt, false);
            }
            if (request->has_up()) {
                Octane::float_3 up = {(float)request->up().x(), (float)request->up().y(), (float)request->up().z()};
                cam->setPinValue(Octane::P_UP, up, false);
            }
            Octane::ApiChangeManager::update();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
private:
    void setDefaultCamera(livelinkapi::CameraState* response) {
        auto* pos = response->mutable_position();
        pos->set_x(5); pos->set_y(3); pos->set_z(5);
        auto* target = response->mutable_target();
        target->set_x(0); target->set_y(0); target->set_z(0);
        auto* up = response->mutable_up();
        up->set_x(0); up->set_y(1); up->set_z(0);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// StreamCallbackService — server-streaming RPC for render image callbacks
//
// Each connected client gets its own CallbackSubscription. SDK callbacks
// push events into the dispatcher, which fans out to all subscriptions.
// The stream loop pops events and writes them as proto messages.
// ═══════════════════════════════════════════════════════════════════════════
class StreamCallbackServiceImpl final : public octaneapi::StreamCallbackService::Service {
    static constexpr const char* SVC = "StreamCallbackService";
public:
    grpc::Status callbackChannel(grpc::ServerContext* context, const google::protobuf::Empty*,
        grpc::ServerWriter<octaneapi::StreamCallbackRequest>* writer) override {
        try {
            ServerLog::instance().req(SVC, "callbackChannel");

            CallbackSubscription subscription(200);
            CallbackDispatcher::Instance().Subscribe(&subscription);

            uint64_t eventsWritten = 0;

            while (!context->IsCancelled()) {
                CallbackEvent event;
                if (!subscription.WaitAndPop(event, std::chrono::milliseconds(200))) {
                    continue;
                }

                octaneapi::StreamCallbackRequest msg;
                switch (event.type) {
                    case CallbackEventType::NewImage: {
                        auto* data = msg.mutable_newimage();
                        data->set_user_data(event.userData);
                        break;
                    }
                    case CallbackEventType::NewStatistics: {
                        auto* data = msg.mutable_newstatistics();
                        data->set_user_data(event.userData);
                        break;
                    }
                    case CallbackEventType::RenderFailure: {
                        auto* data = msg.mutable_renderfailure();
                        data->set_user_data(event.userData);
                        break;
                    }
                    case CallbackEventType::ProjectChanged: {
                        auto* data = msg.mutable_projectmanagerchanged();
                        data->set_user_data(event.userData);
                        break;
                    }
                    default:
                        continue;
                }

                if (!writer->Write(msg)) {
                    break; // client disconnected
                }
                ++eventsWritten;
            }

            CallbackDispatcher::Instance().Unsubscribe(&subscription);

            std::string detail = "client disconnected, " + std::to_string(eventsWritten) + " events sent";
            ServerLog::instance().res(SVC, "callbackChannel", detail);
            return grpc::Status::OK;

        } catch (const std::exception& e) {
            std::string msg = std::string(SVC) + ".callbackChannel: " + e.what();
            ServerLog::instance().err(SVC, "callbackChannel", msg);
            return grpc::Status(grpc::StatusCode::INTERNAL, msg);
        } catch (...) {
            std::string msg = std::string(SVC) + ".callbackChannel: unknown exception";
            ServerLog::instance().err(SVC, "callbackChannel", msg);
            return grpc::Status(grpc::StatusCode::INTERNAL, msg);
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiItemService (apinodesystem_3) — get/set attributes
// ═══════════════════════════════════════════════════════════════════════════
class ItemServiceImpl final : public octaneapi::ApiItemService::Service {
    static constexpr const char* SVC = "ApiItemService";
public:
    grpc::Status isGraph(grpc::ServerContext*, const octaneapi::ApiItem::isGraphRequest* request,
        octaneapi::ApiItem::isGraphResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            response->set_result(item->isGraph());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status isNode(grpc::ServerContext*, const octaneapi::ApiItem::isNodeRequest* request,
        octaneapi::ApiItem::isNodeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            response->set_result(item->isNode());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status name(grpc::ServerContext*, const octaneapi::ApiItem::nameRequest* request,
        octaneapi::ApiItem::nameResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            const char* n = item->name();
            response->set_result(n ? n : "");
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setName(grpc::ServerContext*, const octaneapi::ApiItem::setNameRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            item->setName(request->name().c_str());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status outType(grpc::ServerContext*, const octaneapi::ApiItem::outTypeRequest* request,
        octaneapi::ApiItem::outTypeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            response->set_result(static_cast<octaneapi::NodePinType>(item->outType()));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status uniqueId(grpc::ServerContext*, const octaneapi::ApiItem::uniqueIdRequest* request,
        octaneapi::ApiItem::uniqueIdResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            response->set_result(item->uniqueId());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status destroy(grpc::ServerContext*, const octaneapi::ApiItem::destroyRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            uint64_t handle = request->objectptr().handle();
            item->destroy();
            sHandleRegistry->Unregister(handle);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status evaluate(grpc::ServerContext*, const octaneapi::ApiItem::evaluateRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            auto* node = item->toNode();
            if (!node) {
                std::ostringstream oss;
                oss << "item " << request->objectptr().handle() << " is not a node (evaluate requires a node)";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            node->evaluate();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status deleteUnconnectedItems(grpc::ServerContext*, const octaneapi::ApiItem::deleteUnconnectedItemsRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            item->deleteUnconnectedItems();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Attribute get/set by ID ───────────────────────────────────────
    grpc::Status getValueByAttrID(grpc::ServerContext*, const octaneapi::ApiItem::getValueByIDRequest* request,
        octaneapi::ApiItem::getValueResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->item_ref().handle(), SVC, __func__, status);
            if (!item) return status;

            Octane::AttributeId attrId = static_cast<Octane::AttributeId>(request->attribute_id());
            if (!item->hasAttr(attrId)) {
                std::ostringstream oss;
                oss << "attribute " << static_cast<int>(attrId) << " not found on item " << request->item_ref().handle();
                return failNotFound(SVC, __func__, request->item_ref().handle(), "attribute");
            }

            // Get attribute type to dispatch correctly
            const Octane::ApiAttributeInfo& info = item->attrInfo(attrId);
            switch (info.mType) {
                case Octane::AT_BOOL: {
                    bool v; item->get(attrId, v);
                    response->set_bool_value(v);
                    break;
                }
                case Octane::AT_INT: {
                    int32_t v; item->get(attrId, v);
                    response->set_int_value(v);
                    break;
                }
                case Octane::AT_INT2: {
                    Octane::int32_2 v; item->get(attrId, v);
                    auto* r = response->mutable_int2_value();
                    r->set_x(v.x); r->set_y(v.y);
                    break;
                }
                case Octane::AT_INT3: {
                    Octane::int32_3 v; item->get(attrId, v);
                    auto* r = response->mutable_int3_value();
                    r->set_x(v.x); r->set_y(v.y); r->set_z(v.z);
                    break;
                }
                case Octane::AT_INT4: {
                    Octane::int32_4 v; item->get(attrId, v);
                    auto* r = response->mutable_int4_value();
                    r->set_x(v.x); r->set_y(v.y); r->set_z(v.z); r->set_w(v.w);
                    break;
                }
                case Octane::AT_LONG: {
                    int64_t v; item->get(attrId, v);
                    response->set_long_value(v);
                    break;
                }
                case Octane::AT_LONG2: {
                    Octane::int64_2 v; item->get(attrId, v);
                    auto* r = response->mutable_long2_value();
                    r->set_x(v.x); r->set_y(v.y);
                    break;
                }
                case Octane::AT_FLOAT: {
                    float v; item->get(attrId, v);
                    response->set_float_value(v);
                    break;
                }
                case Octane::AT_FLOAT2: {
                    Octane::float_2 v; item->get(attrId, v);
                    auto* r = response->mutable_float2_value();
                    r->set_x(v.x); r->set_y(v.y);
                    break;
                }
                case Octane::AT_FLOAT3: {
                    Octane::float_3 v; item->get(attrId, v);
                    auto* r = response->mutable_float3_value();
                    r->set_x(v.x); r->set_y(v.y); r->set_z(v.z);
                    break;
                }
                case Octane::AT_FLOAT4: {
                    Octane::float_4 v; item->get(attrId, v);
                    auto* r = response->mutable_float4_value();
                    r->set_x((float)v.x); r->set_y((float)v.y); r->set_z((float)v.z); r->set_w((float)v.w);
                    break;
                }
                case Octane::AT_MATRIX: {
                    Octane::MatrixF v; item->get(attrId, v);
                    auto* r = response->mutable_matrix_value();
                    // SDK MatrixF: Vec4<float> m[3] (3 rows of Vec4)
                    for (int row = 0; row < 3; ++row) {
                        auto* f4 = r->add_m();
                        f4->set_x((float)v.m[row].x);
                        f4->set_y((float)v.m[row].y);
                        f4->set_z((float)v.m[row].z);
                        f4->set_w((float)v.m[row].w);
                    }
                    break;
                }
                case Octane::AT_STRING:
                case Octane::AT_FILENAME: {
                    const char* v = nullptr; item->get(attrId, v);
                    response->set_string_value(v ? v : "");
                    break;
                }
                default: {
                    std::ostringstream oss;
                    oss << "unsupported attribute type " << static_cast<int>(info.mType)
                        << " for attribute " << static_cast<int>(attrId);
                    return failInvalidArg(SVC, __func__, oss.str());
                }
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status setValueByAttrID(grpc::ServerContext*, const octaneapi::ApiItem::setValueByIDRequest* request,
        octaneapi::ApiItem::setValueResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->item_ref().handle(), SVC, __func__, status);
            if (!item) return status;

            Octane::AttributeId attrId = static_cast<Octane::AttributeId>(request->attribute_id());
            bool evaluate = request->has_evaluate() ? request->evaluate() : true;

            switch (request->value_case()) {
                case octaneapi::ApiItem::setValueByIDRequest::kBoolValue:
                    item->set(attrId, request->bool_value(), evaluate);
                    break;
                case octaneapi::ApiItem::setValueByIDRequest::kIntValue:
                    item->set(attrId, request->int_value(), evaluate);
                    break;
                case octaneapi::ApiItem::setValueByIDRequest::kInt2Value: {
                    Octane::int32_2 v = {request->int2_value().x(), request->int2_value().y()};
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kInt3Value: {
                    Octane::int32_3 v = {request->int3_value().x(), request->int3_value().y(), request->int3_value().z()};
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kInt4Value: {
                    Octane::int32_4 v = {request->int4_value().x(), request->int4_value().y(), request->int4_value().z(), request->int4_value().w()};
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kLongValue:
                    item->set(attrId, (int64_t)request->long_value(), evaluate);
                    break;
                case octaneapi::ApiItem::setValueByIDRequest::kLong2Value: {
                    Octane::int64_2 v = {request->long2_value().x(), request->long2_value().y()};
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kFloatValue:
                    item->set(attrId, request->float_value(), evaluate);
                    break;
                case octaneapi::ApiItem::setValueByIDRequest::kFloat2Value: {
                    Octane::float_2 v = {request->float2_value().x(), request->float2_value().y()};
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kFloat3Value: {
                    Octane::float_3 v = {(float)request->float3_value().x(), (float)request->float3_value().y(), (float)request->float3_value().z()};
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kFloat4Value: {
                    Octane::float_4 v = Octane::float_4::make(
                        (float)request->float4_value().x(),
                        (float)request->float4_value().y(),
                        (float)request->float4_value().z(),
                        (float)request->float4_value().w());
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kMatrixValue: {
                    Octane::MatrixF v = Octane::MatrixF::zero();
                    const auto& mv = request->matrix_value();
                    // Proto MatrixF: repeated float_4 m (3 rows of Vec4)
                    for (int row = 0; row < mv.m_size() && row < 3; ++row) {
                        const auto& r = mv.m(row);
                        v.m[row] = Octane::float_4::make(r.x(), r.y(), r.z(), r.w());
                    }
                    item->set(attrId, v, evaluate);
                    break;
                }
                case octaneapi::ApiItem::setValueByIDRequest::kStringValue:
                    item->set(attrId, request->string_value().c_str(), evaluate);
                    break;
                default:
                    response->set_success(false);
                    response->set_error_message("no value provided in setValueByAttrID request");
                    return grpc::Status::OK;
            }
            // Workaround: force evaluate after setting attributes that change node structure.
            // - A_FILENAME/A_RELOAD: mesh needs evaluate to load OBJ and populate material pin
            // - A_PIN_COUNT: group/node needs evaluate to materialize dynamic pins
            // ChangeManager::update() alone is not sufficient.
            if (attrId == Octane::A_FILENAME || attrId == Octane::A_RELOAD || attrId == Octane::A_PIN_COUNT) {
                auto* node = item->toNode();
                if (node) {
                    Octane::ApiChangeManager::update();
                    node->evaluate();
                }
            }
            if (evaluate) {
                Octane::ApiChangeManager::update();
            }

            response->set_success(true);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Attribute introspection ───────────────────────────────────────
    grpc::Status hasAttr(grpc::ServerContext*, const octaneapi::ApiItem::hasAttrRequest* request,
        octaneapi::ApiItem::hasAttrResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            response->set_result(item->hasAttr(static_cast<Octane::AttributeId>(request->id())));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status attrCount(grpc::ServerContext*, const octaneapi::ApiItem::attrCountRequest* request,
        octaneapi::ApiItem::attrCountResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            response->set_result(item->attrCount());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status attrIdIx(grpc::ServerContext*, const octaneapi::ApiItem::attrIdIxRequest* request,
        octaneapi::ApiItem::attrIdIxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            uint32_t index = request->index();
            if (index >= item->attrCount()) {
                std::ostringstream oss;
                oss << "attribute index " << index << " out of range (attrCount is " << item->attrCount() << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            response->set_result(static_cast<octaneapi::AttributeId>(item->attrIdIx(index)));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status attrInfoIx(grpc::ServerContext*, const octaneapi::ApiItem::attrInfoIxRequest* request,
        octaneapi::ApiItem::attrInfoIxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            uint32_t index = request->index();
            if (index >= item->attrCount()) {
                std::ostringstream oss;
                oss << "attribute index " << index << " out of range (attrCount is " << item->attrCount() << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            const Octane::ApiAttributeInfo& info = item->attrInfoIx(index);
            packAttrInfo(info, response->mutable_result());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status attrInfo(grpc::ServerContext*, const octaneapi::ApiItem::attrInfoRequest* request,
        octaneapi::ApiItem::attrInfoResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            Octane::AttributeId attrId = static_cast<Octane::AttributeId>(request->id());
            if (!item->hasAttr(attrId)) {
                return failNotFound(SVC, __func__, request->objectptr().handle(), "attribute");
            }
            const Octane::ApiAttributeInfo& info = item->attrInfo(attrId);
            packAttrInfo(info, response->mutable_result());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Animation ─────────────────────────────────────────────────────
    grpc::Status isAnimated(grpc::ServerContext*, const octaneapi::ApiItem::isAnimatedRequest* request,
        octaneapi::ApiItem::isAnimatedResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            response->set_result(item->isAnimated(static_cast<Octane::AttributeId>(request->id())));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

private:
    // Helper to pack SDK ApiAttributeInfo into proto message
    static void packAttrInfo(const Octane::ApiAttributeInfo& info, octaneapi::ApiAttributeInfo* out) {
        out->set_id(static_cast<octaneapi::AttributeId>(info.mId));
        out->set_type(static_cast<octaneapi::AttributeType>(info.mType));
        out->set_isarray(info.mIsArray);
        out->set_filenameattribute(static_cast<octaneapi::AttributeId>(info.mFileNameAttribute));
        out->set_packageattribute(static_cast<octaneapi::AttributeId>(info.mPackageAttribute));
        out->set_description(info.mDescription ? info.mDescription : "");
        auto* di = out->mutable_defaultints();
        di->set_x(info.mDefaultInts.x); di->set_y(info.mDefaultInts.y);
        di->set_z(info.mDefaultInts.z); di->set_w(info.mDefaultInts.w);
        auto* dl = out->mutable_defaultlongs();
        dl->set_x(info.mDefaultLongs.x); dl->set_y(info.mDefaultLongs.y);
        auto* df = out->mutable_defaultfloats();
        df->set_x(info.mDefaultFloats.x); df->set_y(info.mDefaultFloats.y);
        df->set_z(info.mDefaultFloats.z); df->set_w(info.mDefaultFloats.w);
        out->set_defaultstring(info.mDefaultString ? info.mDefaultString : "");
        out->set_minversion(info.mMinVersion);
        out->set_endversion(info.mEndVersion);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiItemArrayService (apinodesystem_1) — array iteration for scene tree
// ═══════════════════════════════════════════════════════════════════════════
class ItemArrayServiceImpl final : public octaneapi::ApiItemArrayService::Service {
    static constexpr const char* SVC = "ApiItemArrayService";
public:
    grpc::Status size(grpc::ServerContext*, const octaneapi::ApiItemArray::sizeRequest* request,
        octaneapi::ApiItemArray::sizeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* arr = requireArray(request->objectptr().handle(), SVC, __func__, status);
            if (!arr) return status;
            response->set_result(static_cast<uint32_t>(arr->size()));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status get(grpc::ServerContext*, const octaneapi::ApiItemArray::getRequest* request,
        octaneapi::ApiItemArray::getResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* arr = requireArray(request->objectptr().handle(), SVC, __func__, status);
            if (!arr) return status;
            uint32_t index = request->index();
            uint32_t sz = static_cast<uint32_t>(arr->size());
            if (index >= sz) {
                std::ostringstream oss;
                oss << "index " << index << " out of bounds (array size is " << sz << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            auto* item = arr->get(index);
            if (item) {
                uint64_t handle = sHandleRegistry->Register(item);
                response->mutable_result()->set_handle(handle);
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiNodeService (apinodesystem_7) — create, connect, pin operations
// ═══════════════════════════════════════════════════════════════════════════
class NodeServiceImpl final : public octaneapi::ApiNodeService::Service {
    static constexpr const char* SVC = "ApiNodeService";
public:
    // ── Node creation ─────────────────────────────────────────────────
    grpc::Status create(grpc::ServerContext*, const octaneapi::ApiNode::createRequest* request,
        octaneapi::ApiNode::createResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            uint64_t graphHandle = request->ownergraph().handle();
            grpc::Status status;
            auto* graph = requireGraph(graphHandle, SVC, __func__, status);
            if (!graph) return status;

            Octane::NodeType nodeType = static_cast<Octane::NodeType>(request->type());
            bool configurePins = request->configurepins();

            Octane::ApiNode* node = Octane::ApiNode::create(nodeType, *graph, configurePins);
            if (!node) {
                std::ostringstream oss;
                oss << "failed to create node of type " << static_cast<int>(nodeType)
                    << " in graph " << graphHandle;
                return failPrecondition(SVC, __func__, oss.str());
            }
            uint64_t handle = sHandleRegistry->Register(node);
            response->mutable_result()->set_handle(handle);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Node type ─────────────────────────────────────────────────────
    grpc::Status type(grpc::ServerContext*, const octaneapi::ApiNode::typeRequest* request,
        octaneapi::ApiNode::typeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;
            response->set_result(static_cast<octaneapi::NodeType>(node->type()));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Pin introspection ─────────────────────────────────────────────
    grpc::Status pinCount(grpc::ServerContext*, const octaneapi::ApiNode::pinCountRequest* request,
        octaneapi::ApiNode::pinCountResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;
            response->set_result(node->pinCount());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status pinTypeIx(grpc::ServerContext*, const octaneapi::ApiNode::pinTypeIxRequest* request,
        octaneapi::ApiNode::pinTypeIxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;
            uint32_t index = request->index();
            if (index >= node->pinCount()) {
                std::ostringstream oss;
                oss << "pin index " << index << " out of range (pinCount is " << node->pinCount() << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            response->set_result(static_cast<octaneapi::NodePinType>(node->pinTypeIx(index)));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status pinNameIx(grpc::ServerContext*, const octaneapi::ApiNode::pinNameIxRequest* request,
        octaneapi::ApiNode::pinNameIxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;
            uint32_t index = request->index();
            if (index >= node->pinCount()) {
                std::ostringstream oss;
                oss << "pin index " << index << " out of range (pinCount is " << node->pinCount() << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            const char* name = node->pinNameIx(index);
            response->set_result(name ? name : "");
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Connections ───────────────────────────────────────────────────
    grpc::Status connectTo1(grpc::ServerContext*, const octaneapi::ApiNode::connectTo1Request* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;

            // sourceNode handle 0 = disconnect (null)
            Octane::ApiNode* source = nullptr;
            if (request->sourcenode().handle() != 0) {
                source = requireNode(request->sourcenode().handle(), SVC, __func__, status);
                if (!source) return status;
            }

            const std::string& pinName = request->pinname();
            if (pinName.empty()) {
                return failInvalidArg(SVC, __func__, "pinName is empty");
            }

            node->connectTo(pinName.c_str(), source, request->evaluate(), request->docyclecheck());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status connectToIx(grpc::ServerContext*, const octaneapi::ApiNode::connectToIxRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;

            Octane::ApiNode* source = nullptr;
            if (request->sourcenode().handle() != 0) {
                source = requireNode(request->sourcenode().handle(), SVC, __func__, status);
                if (!source) return status;
            }

            // Don't bounds-check pinIdx — SDK handles it. Mesh nodes may have
            // pins before pinCount() reflects them (e.g. material slot on empty mesh).
            node->connectToIx(request->pinidx(), source, request->evaluate(), request->docyclecheck());
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status connectedNode(grpc::ServerContext*, const octaneapi::ApiNode::connectedNodeRequest* request,
        octaneapi::ApiNode::connectedNodeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;
            Octane::PinId pinId = static_cast<Octane::PinId>(request->pinid());
            Octane::ApiNode* connected = node->connectedNode(pinId, request->enterwrappernode());
            if (connected) {
                uint64_t handle = sHandleRegistry->Register(connected);
                response->mutable_result()->set_handle(handle);
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status connectedNodeIx(grpc::ServerContext*, const octaneapi::ApiNode::connectedNodeIxRequest* request,
        octaneapi::ApiNode::connectedNodeIxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;

            uint32_t pinIx = request->pinix();
            Octane::ApiNode* connected = node->connectedNodeIx(pinIx, request->enterwrappernode());
            if (connected) {
                uint64_t handle = sHandleRegistry->Register(connected);
                response->mutable_result()->set_handle(handle);
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Owned items (pin-owned children) ──────────────────────────────
    grpc::Status ownedItemIx(grpc::ServerContext*, const octaneapi::ApiNode::ownedItemIxRequest* request,
        octaneapi::ApiNode::ownedItemIxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;

            Octane::ApiItem* owned = node->ownedItemIx(request->pinix());
            if (owned) {
                uint64_t handle = sHandleRegistry->Register(owned);
                response->mutable_result()->set_handle(handle);
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiNodeGraphService (apinodesystem_6) — scene tree traversal
// ═══════════════════════════════════════════════════════════════════════════
class NodeGraphServiceImpl final : public octaneapi::ApiNodeGraphService::Service {
    static constexpr const char* SVC = "ApiNodeGraphService";
public:
    grpc::Status getOwnedItems(grpc::ServerContext*, const octaneapi::ApiNodeGraph::getOwnedItemsRequest* request,
        octaneapi::ApiNodeGraph::getOwnedItemsResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* graph = requireGraph(request->objectptr().handle(), SVC, __func__, status);
            if (!graph) return status;

            // Heap-allocate the array — registry takes ownership
            auto* itemArray = new Octane::ApiItemArray();
            graph->getOwnedItems(*itemArray);
            uint64_t handle = sHandleRegistry->RegisterArray(itemArray);
            response->mutable_list()->set_handle(handle);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status findItemsByName(grpc::ServerContext*, const octaneapi::ApiNodeGraph::findItemsByNameRequest* request,
        octaneapi::ApiNodeGraph::findItemsByNameResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* graph = requireGraph(request->objectptr().handle(), SVC, __func__, status);
            if (!graph) return status;
            auto* itemArray = new Octane::ApiItemArray();
            graph->findItemsByName(request->name().c_str(), *itemArray, request->recurse());
            uint64_t handle = sHandleRegistry->RegisterArray(itemArray);
            response->mutable_list()->set_handle(handle);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status findNodes(grpc::ServerContext*, const octaneapi::ApiNodeGraph::findNodesRequest* request,
        octaneapi::ApiNodeGraph::findNodesResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* graph = requireGraph(request->objectptr().handle(), SVC, __func__, status);
            if (!graph) return status;

            Octane::NodeType nodeType = static_cast<Octane::NodeType>(request->type());
            Octane::ApiNodeArray nodeArray;
            graph->findNodes(nodeType, nodeArray, request->recurse());

            // Convert ApiNodeArray → ApiItemArray for the handle registry
            auto* itemArray = new Octane::ApiItemArray();
            size_t count = nodeArray.size();
            if (count > 0) {
                itemArray->init(count);
                Octane::ApiItem** items = itemArray->items();
                for (size_t i = 0; i < count; ++i) {
                    items[i] = nodeArray.get(i);
                }
            }
            uint64_t handle = sHandleRegistry->RegisterArray(itemArray);
            response->mutable_list()->set_handle(handle);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status copyItemTree(grpc::ServerContext*, const octaneapi::ApiNodeGraph::copyItemTreeRequest* request,
        octaneapi::ApiNodeGraph::copyItemTreeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* graph = requireGraph(request->objectptr().handle(), SVC, __func__, status);
            if (!graph) return status;
            auto* rootItem = requireItem(request->rootitem().handle(), SVC, __func__, status);
            if (!rootItem) return status;
            Octane::ApiItem* copy = graph->copyItemTree(*rootItem);
            if (copy) {
                uint64_t handle = sHandleRegistry->Register(copy);
                response->mutable_result()->set_handle(handle);
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status type1(grpc::ServerContext*, const octaneapi::ApiNodeGraph::typeRequest* request,
        octaneapi::ApiNodeGraph::typeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* graph = requireGraph(request->objectptr().handle(), SVC, __func__, status);
            if (!graph) return status;
            response->set_result(static_cast<octaneapi::NodeGraphType>(graph->type()));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// GrpcServer
// ═══════════════════════════════════════════════════════════════════════════

GrpcServer::GrpcServer(uint16_t port)
    : mPort(port)
    , mHandleRegistry(std::make_unique<HandleRegistry>())
{
    sHandleRegistry = mHandleRegistry.get();
}

GrpcServer::~GrpcServer() {
    StopServer();
    sHandleRegistry = nullptr;
}

void GrpcServer::RunServer() {
    std::string serverAddress = "0.0.0.0:" + std::to_string(mPort);

    // Create all service implementations
    InfoServiceImpl               infoService;
    ProjectManagerServiceImpl     projectManagerService;
    ChangeManagerServiceImpl      changeManagerService;
    RenderEngineServiceImpl       renderEngineService;
    LiveLinkServiceImpl           liveLinkService;
    StreamCallbackServiceImpl     streamCallbackService;
    ItemServiceImpl               itemService;
    ItemArrayServiceImpl          itemArrayService;
    NodeServiceImpl               nodeService;
    NodeGraphServiceImpl          nodeGraphService;

    // Build and start the server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());

    // Add logging interceptor — logs every RPC call to log_serv.log
    std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;
    interceptors.push_back(std::make_unique<LoggingInterceptorFactory>());
    builder.experimental().SetInterceptorCreators(std::move(interceptors));

    // Register all services
    builder.RegisterService(&infoService);
    builder.RegisterService(&projectManagerService);
    builder.RegisterService(&changeManagerService);
    builder.RegisterService(&renderEngineService);
    builder.RegisterService(&liveLinkService);
    builder.RegisterService(&streamCallbackService);
    builder.RegisterService(&itemService);
    builder.RegisterService(&itemArrayService);
    builder.RegisterService(&nodeService);
    builder.RegisterService(&nodeGraphService);

    // Configure message sizes (match Octane's settings)
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);

    // Allow frequent client keepalive pings (octaneWebR sends every 10-20s).
    // Default gRPC server rejects pings more frequent than 5 minutes.
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

    mServer = builder.BuildAndStart();
    if (!mServer) {
        std::cerr << "[OctaneServGrpc] ERROR: Failed to start gRPC server on " << serverAddress << std::endl;
        return;
    }

    mRunning = true;
    std::cout << "[OctaneServGrpc] gRPC server listening on " << serverAddress << std::endl;
    std::cout << "[OctaneServGrpc] Services: ApiInfo, ProjectManager, ChangeManager, RenderEngine, LiveLink, StreamCallback, Item, ItemArray, Node, NodeGraph" << std::endl;
    std::cout << "[OctaneServGrpc] Hardening: GRPC_SAFE on all RPCs, validated lookups, input bounds, SEH protection" << std::endl;

    // Block until StopServer() is called
    mServer->Wait();
    mRunning = false;

    std::cout << "[OctaneServGrpc] gRPC server stopped." << std::endl;
}

void GrpcServer::StopServer() {
    if (mServer) {
        mServer->Shutdown();
    }
}

} // namespace OctaneServ
