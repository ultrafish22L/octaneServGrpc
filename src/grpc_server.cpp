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

#include "apilogmanager.h"
API_RLOG_USE(serv)

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
#include "apinodepininfohelper.grpc.pb.h"
#include "sharedsurfaceframe.grpc.pb.h"

// Octane shared surface API (dxSS path)
#include "apisharedsurface.h"

#ifdef _WIN32
#include <windows.h>
#include <map>
#endif

namespace OctaneServ {

// Shared handle registry — all services access the same instance
static HandleRegistry* sHandleRegistry = nullptr;

// Maximum recursion depth for auto-registering pin children.
// Octane's deepest auto-created pin trees are ~8 deep (RT→kernel→settings);
// 22 gives 3x headroom to avoid silent truncation.
static constexpr int MAX_PIN_TREE_DEPTH = 22;

// Pin info handles: encode node uniqueId + pin index into a single uint64.
// High 48 bits = node handle, low 16 bits = pin index. No cache needed —
// decode at lookup time, get pin info from live SDK node.
static uint64_t encodePinInfoHandle(uint64_t nodeHandle, uint32_t pinIndex) {
    return (nodeHandle << 16) | (pinIndex & 0xFFFF);
}
static void decodePinInfoHandle(uint64_t handle, uint64_t& nodeHandle, uint32_t& pinIndex) {
    nodeHandle = handle >> 16;
    pinIndex = static_cast<uint32_t>(handle & 0xFFFF);
}

// ═══════════════════════════════════════════════════════════════════════════
// Hardening infrastructure
// ═══════════════════════════════════════════════════════════════════════════

// GRPC_SAFE_BEGIN / GRPC_SAFE_END: Wraps every RPC method body.
// Catches all C++ exceptions and returns a descriptive gRPC error.
// The server NEVER crashes from a bad request.
//
// On Windows, /EHa is enabled in CMakeLists.txt so catch(...) also catches
// SEH exceptions (access violations, null dereferences). We can't use
// __try/__except directly because it conflicts with C++ objects that have
// destructors (grpc::Status, std::string) in the same function (MSVC C2712).
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

// Walk a node's pin children recursively and register all of them.
// Prevents "handle not found" errors when the client calls attrInfo
// on auto-created pin children without first calling connectedNodeIx.
static void registerPinChildrenRecursive(Octane::ApiNode* node, int depth = 0) {
    if (!node) return;
    if (depth > MAX_PIN_TREE_DEPTH) {
        ServerLog::instance().log("WRN", "HandleReg", "registerPinChildren",
            "depth limit " + std::to_string(MAX_PIN_TREE_DEPTH) + " reached — deeper pin children will not be auto-registered");
        return;
    }
    for (uint32_t i = 0; i < node->pinCount(); ++i) {
        Octane::ApiNode* child = node->connectedNodeIx(i, false);
        if (child) {
            sHandleRegistry->Register(child);
            registerPinChildrenRecursive(child, depth + 1);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Safe enum normalization helpers — validate SDK enum values before casting
// to proto enums. Unknown values map to the _UNKNOWN variant (value 0).
// Prevents raw SDK integers from leaking to clients as stringified numbers.
// ═══════════════════════════════════════════════════════════════════════════

static octaneapi::NodeType safeNodeType(int raw, const char* ctx = nullptr) {
    if (octaneapi::NodeType_IsValid(raw))
        return static_cast<octaneapi::NodeType>(raw);
    ServerLog::instance().log("WRN", "EnumGuard", ctx ? ctx : "safeNodeType",
        "normalized unknown SDK NodeType " + std::to_string(raw) + " → NT_UNKNOWN");
    return octaneapi::NT_UNKNOWN;
}

static octaneapi::NodePinType safePinType(int raw, const char* ctx = nullptr) {
    if (octaneapi::NodePinType_IsValid(raw))
        return static_cast<octaneapi::NodePinType>(raw);
    ServerLog::instance().log("WRN", "EnumGuard", ctx ? ctx : "safePinType",
        "normalized unknown SDK NodePinType " + std::to_string(raw) + " → PT_UNKNOWN");
    return octaneapi::PT_UNKNOWN;
}

static octaneapi::NodeGraphType safeGraphType(int raw, const char* ctx = nullptr) {
    if (octaneapi::NodeGraphType_IsValid(raw))
        return static_cast<octaneapi::NodeGraphType>(raw);
    ServerLog::instance().log("WRN", "EnumGuard", ctx ? ctx : "safeGraphType",
        "normalized unknown SDK NodeGraphType " + std::to_string(raw) + " → GT_UNKNOWN");
    return octaneapi::GT_UNKNOWN;
}

static octaneapi::ImageType safeImageType(int raw, const char* ctx = nullptr) {
    if (octaneapi::ImageType_IsValid(raw))
        return static_cast<octaneapi::ImageType>(raw);
    ServerLog::instance().log("WRN", "EnumGuard", ctx ? ctx : "safeImageType",
        "normalized unknown SDK ImageType " + std::to_string(raw) + " → IMAGE_TYPE_LDR_RGBA");
    return octaneapi::IMAGE_TYPE_LDR_RGBA;
}

// ═══════════════════════════════════════════════════════════════════════════
// ApiInfoService — SDK metadata and type catalog
//
// Exposes Octane version info, node type definitions, pin metadata, and
// attribute metadata. Called by the MCP layer at startup for type discovery.
// All methods are read-only — no scene state is modified.
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
            r->set_type(safeNodeType(static_cast<int>(info->mType), "nodeInfo"));
            r->set_description(info->mDescription ? info->mDescription : "");
            r->set_outtype(safePinType(static_cast<int>(info->mOutType), "nodeInfo"));
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
            // nodePinInfo returns an ObjectRef in the proto, but pin info is
            // accessed through ApiNode::pinInfoIx on the live node. Return
            // handle 0 — the MCP layer calls getApiNodePinInfo for pin data.
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
                cg->add_data(safeGraphType(static_cast<int>(compatGraphs[i]), "getCompatibleTypes"));
            }
            response->set_compatgraphssize(static_cast<uint32_t>(compatGraphsSize));

            auto* cn = response->mutable_compatnodes();
            for (size_t i = 0; i < compatNodesSize; ++i) {
                cn->add_data(safeNodeType(static_cast<int>(compatNodes[i]), "getCompatibleTypes"));
            }
            response->set_compatnodessize(static_cast<uint32_t>(compatNodesSize));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiProjectManagerService — scene lifecycle
//
// Load, save, and reset Octane projects (.orbx). Provides access to the
// root node graph (entry point for scene traversal). The handle registry
// is cleared on loadProject/resetProject to prevent stale SDK pointers.
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
// ApiChangeManagerService — scene evaluation trigger
//
// Single RPC: update(). Flushes all pending attribute changes to the render
// engine. Must be called after batched set() calls with evaluate=false.
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
// ApiRenderEngineService — GPU render control and diagnostics
//
// Start/stop/pause rendering, device info, VRAM usage, geometry/texture
// statistics, clay mode, save image, grab pixel buffers, enabled AOVs,
// shared surface output configuration. Largest service in the server.
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
            // ClayMode: 0=none, 1=grey, 2=color, 3=color+wireframe
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
            int mode = request->mode();
            // SubSampleMode: 0=none, 1=2x2, 2=4x4, 3=8x8
            if (mode < 0 || mode > 3) {
                std::ostringstream oss;
                oss << "sub-sample mode " << mode << " is not valid (valid: 0=none, 1=2x2, 2=4x4, 3=8x8)";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            Octane::ApiRenderEngine::setSubSampleMode(static_cast<Octane::SubSampleMode>(mode));
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
            // RenderPriority: 0=LOW, 1=MEDIUM, 2=HIGH
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
            if (!result) {
                ServerLog::instance().err(SVC, __func__,
                    "setRenderTargetNode failed for handle " + std::to_string(handle)
                    + " — node may not be a valid render target (must be NT_RENDERTARGET)");
            }
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
            auto status = validateRenderReady("continueRendering");
            if (!status.ok()) return status;
            Octane::ApiRenderEngine::continueRendering();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status restartRendering(grpc::ServerContext*, const octaneapi::ApiRenderEngine::restartRenderingRequest*,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            auto status = validateRenderReady("restartRendering");
            if (!status.ok()) return status;
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

private:
    // Pre-render validation: check that the scene is renderable before
    // starting/restarting/continuing. Returns OK if ready, or FAILED_PRECONDITION
    // with a diagnostic message listing exactly what's missing.
    static grpc::Status validateRenderReady(const char* method) {
        Octane::ApiNode* rt = Octane::ApiRenderEngine::getRenderTargetNode();
        if (!rt) {
            return failPrecondition(SVC, method,
                "no render target set — call setRenderTargetNode first");
        }
        // Check critical pins: camera (0), geometry (3), kernel (6)
        std::string missing;
        if (!rt->connectedNodeIx(0, false))
            missing += "pin 0 (camera) has no node connected. ";
        if (!rt->connectedNodeIx(3, false))
            missing += "pin 3 (geometry/mesh) has no node connected — scene will render all white. ";
        if (!rt->connectedNodeIx(6, false))
            missing += "pin 6 (kernel) has no node connected. ";

        if (!missing.empty()) {
            return failPrecondition(SVC, method,
                "render target is missing required connections: " + missing
                + "Use connect_nodes to wire the missing nodes.");
        }
        return grpc::Status::OK;
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
    grpc::Status deviceSharedSurfaceInfo(grpc::ServerContext*, const octaneapi::ApiRenderEngine::deviceSharedSurfaceInfoRequest* request,
        octaneapi::ApiRenderEngine::deviceSharedSurfaceInfoResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            uint32_t idx = request->index();
            uint32_t count = Octane::ApiRenderEngine::getDeviceCount();
            if (idx >= count) {
                std::ostringstream oss;
                oss << "device index " << idx << " out of range (device count is " << count << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            auto info = Octane::ApiRenderEngine::deviceSharedSurfaceInfo(idx);
            auto* result = response->mutable_result();
            auto* d3d11 = result->mutable_d3d11();
            d3d11->set_msupported(info.mD3D11.mSupported);
            d3d11->set_madapterluid(info.mD3D11.mAdapterLuid);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setAsyncTonemapRenderPasses(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setAsyncTonemapRenderPassesRequest* request,
        octaneapi::ApiRenderEngine::setAsyncTonemapRenderPassesResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            const auto& protoArr = request->tonemappasses();
            std::vector<Octane::RenderPassId> passes;
            for (int i = 0; i < protoArr.data_size(); ++i) {
                passes.push_back(static_cast<Octane::RenderPassId>(protoArr.data(i)));
            }
            Octane::ApiArray<Octane::RenderPassId> arr;
            arr.mData = passes.data();
            arr.mSize = passes.size();
            bool ok = Octane::ApiRenderEngine::setAsyncTonemapRenderPasses(arr);
            response->set_result(ok);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setSharedSurfaceOutputType(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setSharedSurfaceOutputTypeRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            auto type = static_cast<Octane::SharedSurfaceType>(request->type());
            bool realTime = request->realtime();
            Octane::ApiRenderEngine::setSharedSurfaceOutputType(type, realTime);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status getSharedSurfaceOutputType(grpc::ServerContext*, const octaneapi::ApiRenderEngine::getSharedSurfaceOutputTypeRequest*,
        octaneapi::ApiRenderEngine::getSharedSurfaceOutputTypeResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            auto type = Octane::ApiRenderEngine::getSharedSurfaceOutputType();
            response->set_result(static_cast<octaneapi::SharedSurfaceType>(type));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    // ── Callback registration stubs ─────────────────────────────────
    // SDK callbacks are registered server-side in SdkEngine::RegisterCallbacks().
    // Clients receive events via StreamCallbackService::callbackChannel().
    // These RPCs exist for proto compatibility but are no-ops — return a
    // sentinel callbackId so the client knows registration is server-managed.
    grpc::Status setOnNewImageCallback(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setOnNewImageCallbackRequest*,
        octaneapi::ApiRenderEngine::setOnNewImageCallbackResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_callbackid(1); // server-managed — use callbackChannel()
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setOnNewStatisticsCallback(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setOnNewStatisticsCallbackRequest*,
        octaneapi::ApiRenderEngine::setOnNewStatisticsCallbackResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_callbackid(2); // server-managed — use callbackChannel()
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
    grpc::Status setOnRenderFailureCallback(grpc::ServerContext*, const octaneapi::ApiRenderEngine::setOnRenderFailureCallbackRequest*,
        octaneapi::ApiRenderEngine::setOnRenderFailureCallbackResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            response->set_callbackid(3); // server-managed — use callbackChannel()
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
            if (!result) {
                ServerLog::instance().err(SVC, __func__,
                    "saveImage returned false for path: " + request->fullpath()
                    + " — verify path exists, render is active, and passId " + std::to_string(static_cast<int>(passId)) + " is valid");
            }
            response->set_result(result);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Enabled AOVs ──────────────────────────────────────────────────
    // ── Grab Render Result (pixel data for viewport) ────────────────
    grpc::Status grabRenderResult(grpc::ServerContext*, const octaneapi::ApiRenderEngine::grabRenderResultRequest*,
        octaneapi::ApiRenderEngine::grabRenderResultResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiArray<Octane::ApiRenderImage> renderImages;
            bool result = Octane::ApiRenderEngine::grabRenderResult(renderImages);
            response->set_result(result);

            if (result && renderImages.mSize > 0) {
                auto* arr = response->mutable_renderimages();
                for (size_t i = 0; i < renderImages.mSize; ++i) {
                    const Octane::ApiRenderImage& img = renderImages.mData[i];
                    auto* protoImg = arr->add_data();

                    protoImg->set_type(safeImageType(static_cast<int>(img.mType), "renderImages"));
                    protoImg->set_colorspace(static_cast<octaneapi::NamedColorSpace>(img.mColorSpace));
                    protoImg->set_islinear(img.mIsLinear);
                    auto* sz = protoImg->mutable_size();
                    sz->set_x(img.mSize.x);
                    sz->set_y(img.mSize.y);
                    protoImg->set_pitch(img.mPitch);
                    protoImg->set_renderpassid(static_cast<octaneapi::RenderPassId>(img.mRenderPassId));
                    protoImg->set_tonemappedsamplesperpixel(img.mTonemappedSamplesPerPixel);
                    protoImg->set_calculatedsamplesperpixel(img.mCalculatedSamplesPerPixel);
                    protoImg->set_regionsamplesperpixel(img.mRegionSamplesPerPixel);
                    protoImg->set_maxsamplesperpixel(img.mMaxSamplesPerPixel);

                    // Warn if shared surface mode is on but caller used pixel path
                    if (!img.mBuffer && img.mSharedSurface) {
                        ServerLog::instance().log("WRN", SVC, "grabRenderResult",
                            "image " + std::to_string(i) + " has shared surface but no pixel buffer"
                            " — client should use grabSharedFrame instead");
                    }

                    // Pack pixel buffer
                    if (img.mBuffer && img.mSize.x > 0 && img.mSize.y > 0) {
                        if (img.mPitch == 0) {
                            ServerLog::instance().log("WRN", SVC, "grabRenderResult",
                                "image " + std::to_string(i) + " has zero pitch — skipping");
                            continue;
                        }
                        // Calculate buffer size based on image type
                        size_t bytesPerPixel = 4; // default RGBA8
                        switch (img.mType) {
                            case Octane::IMAGE_TYPE_LDR_RGBA: bytesPerPixel = 4; break;
                            case Octane::IMAGE_TYPE_HDR_RGBA: bytesPerPixel = 16; break; // 4 floats
                            default: bytesPerPixel = 4; break;
                        }
                        // Overflow protection: validate before multiplying
                        if (static_cast<size_t>(img.mPitch) > SIZE_MAX / img.mSize.y / bytesPerPixel) {
                            ServerLog::instance().err(SVC, "grabRenderResult",
                                "image " + std::to_string(i) + " buffer size overflow ("
                                + std::to_string(img.mPitch) + "x" + std::to_string(img.mSize.y)
                                + "x" + std::to_string(bytesPerPixel) + ") — skipping");
                            continue;
                        }
                        size_t bufSize = static_cast<size_t>(img.mPitch) * img.mSize.y * bytesPerPixel;
                        if (bufSize > UINT32_MAX) {
                            ServerLog::instance().err(SVC, "grabRenderResult",
                                "image " + std::to_string(i) + " buffer too large ("
                                + std::to_string(bufSize) + " bytes) — skipping");
                            continue;
                        }
                        auto* buf = protoImg->mutable_buffer();
                        buf->set_data(img.mBuffer, bufSize);
                        buf->set_size(static_cast<uint32_t>(bufSize));
                    }
                }
            }

            // Release so the engine can reuse the buffer
            if (result) {
                Octane::ApiRenderEngine::releaseRenderResult();
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Release Render Result ─────────────────────────────────────────
    grpc::Status releaseRenderResult(grpc::ServerContext*, const octaneapi::ApiRenderEngine::releaseRenderResultRequest*,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
            Octane::ApiRenderEngine::releaseRenderResult();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

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
// LiveLinkService — real-time camera I/O and server health
//
// GetCamera/SetCamera for live viewport camera positioning (used by MCP's
// fit_camera and set_camera). GetServVersion returns build number and
// handle registry diagnostics for verifying the running server matches
// the expected build.
// ═══════════════════════════════════════════════════════════════════════════
// Build number — increment on every code change to verify running code matches build.
static constexpr int SERV_BUILD = 5;

class LiveLinkServiceImpl final : public livelinkapi::LiveLinkService::Service {
    static constexpr const char* SVC = "LiveLinkService";
public:
    grpc::Status GetServVersion(grpc::ServerContext*, const livelinkapi::Empty*,
        livelinkapi::ServVersionResponse* response) override {
        response->set_build(SERV_BUILD);
        // Handle registry diagnostics
        response->set_handle_count(sHandleRegistry->Size());
        response->set_stale_evictions(sHandleRegistry->StaleEvictions());
        response->set_item_count(sHandleRegistry->ItemCount());
        response->set_array_count(sHandleRegistry->ArrayCount());
        // SDK health
        response->set_sdk_ready(SdkEngine::IsReady());
        response->set_sdk_version(SdkEngine::IsReady() ? SdkEngine::GetVersion() : 0);
        response->set_sdk_activated(SdkEngine::IsActivated());
        // Server state
        response->set_log_level(ServerLog::instance().levelName());
        return grpc::Status::OK;
    }

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
                cam->setPinValue(Octane::P_POSITION, pos, true);
            }
            if (request->has_target()) {
                Octane::float_3 tgt = {(float)request->target().x(), (float)request->target().y(), (float)request->target().z()};
                cam->setPinValue(Octane::P_TARGET, tgt, true);
            }
            if (request->has_up()) {
                Octane::float_3 up = {(float)request->up().x(), (float)request->up().y(), (float)request->up().z()};
                float len2 = up.x*up.x + up.y*up.y + up.z*up.z;
                if (len2 < 1e-12f) {
                    up = {0.f, 1.f, 0.f}; // degenerate → default
                } else {
                    float inv = 1.f / std::sqrt(len2);
                    up = {up.x * inv, up.y * inv, up.z * inv}; // normalize
                }
                cam->setPinValue(Octane::P_UP, up, true);
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
// ApiItemService (apinodesystem_3) — item-level attribute operations
//
// Get/set attribute values by ID with full type dispatch (bool, int, int2-4,
// long, float, float2-4, matrix, string). Attribute introspection (hasAttr,
// attrInfo, attrCount). Item lifecycle (destroy, evaluate). The type dispatch
// switch in getValueByAttrID/setValueByAttrID is the most complex code path
// in the server — it handles all 14 SDK attribute types.
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
            response->set_result(safePinType(static_cast<int>(item->outType()), "ApiItem::outType"));
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
            ServerLog::instance().req(SVC, "evaluate",
                "handle=" + std::to_string(request->objectptr().handle()));
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
            ServerLog::instance().req(SVC, "deleteUnconnectedItems",
                "handle=" + std::to_string(request->objectptr().handle()));
            item->deleteUnconnectedItems();
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Attribute get/set by ID ───────────────────────────────────────
    grpc::Status getValueByAttrID(grpc::ServerContext*, const octaneapi::ApiItem::getValueByIDRequest* request,
        octaneapi::ApiItem::getValueResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;

            Octane::AttributeId attrId = static_cast<Octane::AttributeId>(request->attribute_id());
            if (!item->hasAttr(attrId)) {
                // Return empty response — client probes attributes on nodes that may not have them
                return grpc::Status::OK;
            }

            // Get attribute type to dispatch correctly
            // Inner try/catch: some SDK nodes throw on attrInfo() or get() for valid
            // attribute types (e.g. Round Edges children, material layer children).
            // Return empty OK instead of INTERNAL — caller treats empty as "no value".
            try {
            const Octane::ApiAttributeInfo& info = item->attrInfo(attrId);
            switch (info.mType) {
                case Octane::AT_BOOL: {
                    bool v = false; item->get(attrId, v);
                    response->set_bool_value(v);
                    break;
                }
                case Octane::AT_INT: {
                    int32_t v = 0; item->get(attrId, v);
                    response->set_int_value(v);
                    break;
                }
                case Octane::AT_INT2: {
                    Octane::int32_2 v = {0, 0}; item->get(attrId, v);
                    auto* r = response->mutable_int2_value();
                    r->set_x(v.x); r->set_y(v.y);
                    break;
                }
                case Octane::AT_INT3: {
                    Octane::int32_3 v = {0, 0, 0}; item->get(attrId, v);
                    auto* r = response->mutable_int3_value();
                    r->set_x(v.x); r->set_y(v.y); r->set_z(v.z);
                    break;
                }
                case Octane::AT_INT4: {
                    Octane::int32_4 v = {0, 0, 0, 0}; item->get(attrId, v);
                    auto* r = response->mutable_int4_value();
                    r->set_x(v.x); r->set_y(v.y); r->set_z(v.z); r->set_w(v.w);
                    break;
                }
                case Octane::AT_LONG: {
                    int64_t v = 0; item->get(attrId, v);
                    response->set_long_value(v);
                    break;
                }
                case Octane::AT_LONG2: {
                    Octane::int64_2 v = {0, 0}; item->get(attrId, v);
                    auto* r = response->mutable_long2_value();
                    r->set_x(v.x); r->set_y(v.y);
                    break;
                }
                case Octane::AT_FLOAT: {
                    float v = 0.f; item->get(attrId, v);
                    response->set_float_value(v);
                    break;
                }
                case Octane::AT_FLOAT2: {
                    Octane::float_2 v = {0.f, 0.f}; item->get(attrId, v);
                    auto* r = response->mutable_float2_value();
                    r->set_x(v.x); r->set_y(v.y);
                    break;
                }
                case Octane::AT_FLOAT3: {
                    Octane::float_3 v = {0.f, 0.f, 0.f}; item->get(attrId, v);
                    auto* r = response->mutable_float3_value();
                    r->set_x(v.x); r->set_y(v.y); r->set_z(v.z);
                    break;
                }
                case Octane::AT_FLOAT4: {
                    Octane::float_4 v = Octane::float_4::make(0.f, 0.f, 0.f, 0.f); item->get(attrId, v);
                    auto* r = response->mutable_float4_value();
                    r->set_x((float)v.x); r->set_y((float)v.y); r->set_z((float)v.z); r->set_w((float)v.w);
                    break;
                }
                case Octane::AT_MATRIX: {
                    Octane::MatrixF v = Octane::MatrixF::zero(); item->get(attrId, v);
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
            } catch (const std::exception& e) {
                ServerLog::instance().log("WRN", SVC, "getValueByAttrID",
                    "threw for attr " + std::to_string(static_cast<int>(attrId))
                    + " on handle " + std::to_string(request->objectptr().handle())
                    + ": " + e.what());
                return grpc::Status::OK; // empty response = no value
            } catch (...) {
                ServerLog::instance().log("WRN", SVC, "getValueByAttrID",
                    "threw unknown exception for attr " + std::to_string(static_cast<int>(attrId))
                    + " on handle " + std::to_string(request->objectptr().handle()));
                return grpc::Status::OK; // empty response = no value
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status setValueByAttrID(grpc::ServerContext*, const octaneapi::ApiItem::setValueByIDRequest* request,
        octaneapi::ApiItem::setValueResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;

            Octane::AttributeId attrId = static_cast<Octane::AttributeId>(request->attribute_id());
            if (!item->hasAttr(attrId)) {
                std::ostringstream oss;
                oss << "attribute " << static_cast<int>(attrId)
                    << " not supported on handle " << request->objectptr().handle();
                return failInvalidArg(SVC, __func__, oss.str());
            }

            // Type mismatch detection: compare the attribute's SDK type with the
            // oneof value the client sent. Catches errors like sending a float3
            // to an int attribute. Allows float→float4 and int→int4 promotions
            // because the SDK uses float4/int4 internally for value nodes.
            {
                const Octane::ApiAttributeInfo& attrInfo = item->attrInfo(attrId);
                auto sentCase = request->value_case();
                bool mismatch = false;
                std::string sent;
                auto t = attrInfo.mType;

                // Helper: check if SDK type is in a compatible float family or int family
                auto isFloatFamily = [](Octane::AttributeType t) {
                    return t == Octane::AT_FLOAT || t == Octane::AT_FLOAT2
                        || t == Octane::AT_FLOAT3 || t == Octane::AT_FLOAT4;
                };
                auto isIntFamily = [](Octane::AttributeType t) {
                    return t == Octane::AT_INT || t == Octane::AT_INT2
                        || t == Octane::AT_INT3 || t == Octane::AT_INT4;
                };

                switch (sentCase) {
                    case octaneapi::ApiItem::setValueByIDRequest::kBoolValue:
                        if (t != Octane::AT_BOOL) { mismatch = true; sent = "bool"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kIntValue:
                        // Allow int → int/int2/int3/int4 (SDK wraps scalar to int4 internally)
                        if (!isIntFamily(t)) { mismatch = true; sent = "int"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kInt2Value:
                        if (!isIntFamily(t)) { mismatch = true; sent = "int2"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kInt3Value:
                        if (!isIntFamily(t)) { mismatch = true; sent = "int3"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kInt4Value:
                        if (!isIntFamily(t)) { mismatch = true; sent = "int4"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kLongValue:
                        if (t != Octane::AT_LONG && t != Octane::AT_LONG2) { mismatch = true; sent = "long"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kFloatValue:
                        // Allow float → float/float2/float3/float4 (SDK wraps scalar to float4 internally)
                        if (!isFloatFamily(t)) { mismatch = true; sent = "float"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kFloat2Value:
                        if (!isFloatFamily(t)) { mismatch = true; sent = "float2"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kFloat3Value:
                        if (!isFloatFamily(t)) { mismatch = true; sent = "float3"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kFloat4Value:
                        if (!isFloatFamily(t)) { mismatch = true; sent = "float4"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kMatrixValue:
                        if (t != Octane::AT_MATRIX) { mismatch = true; sent = "matrix"; } break;
                    case octaneapi::ApiItem::setValueByIDRequest::kStringValue:
                        if (t != Octane::AT_STRING && t != Octane::AT_FILENAME) { mismatch = true; sent = "string"; } break;
                    default: break;
                }

                if (mismatch) {
                    std::ostringstream oss;
                    oss << "type mismatch on attribute " << static_cast<int>(attrId)
                        << " (handle " << request->objectptr().handle() << "): SDK type is "
                        << static_cast<int>(t) << " but you sent " << sent
                        << ". Use the matching value field for the attribute's type.";
                    return failInvalidArg(SVC, __func__, oss.str());
                }
            }

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
            // Always force evaluate after any attribute set.
            // The SDK's set() with evaluate=false defers changes. Many attributes
            // need an explicit node->evaluate() to take effect (A_FILENAME loads OBJ,
            // A_PIN_COUNT materializes pins, A_TRANSLATION recalculates transform matrix).
            // Rather than maintaining a list of special attributes, just always evaluate.
            {
                Octane::ApiChangeManager::update();
                auto* node = item->toNode();
                if (node) node->evaluate();
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
            // Return empty response if attribute doesn't exist — client probes attributes
            if (!item->hasAttr(attrId)) {
                return grpc::Status::OK;
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

    // ── Position (item's position in its owner graph) ───────────────
    grpc::Status position(grpc::ServerContext*, const octaneapi::ApiItem::positionRequest* request,
        octaneapi::ApiItem::positionResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* item = requireItem(request->objectptr().handle(), SVC, __func__, status);
            if (!item) return status;
            Octane::float_2 pos = item->position();
            auto* r = response->mutable_result();
            r->set_x(pos.x); r->set_y(pos.y);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

private:
    // Helper to pack SDK ApiAttributeInfo into proto message.
    // Normalizes unknown/internal SDK attribute types to AT_UNKNOWN
    // so the client never receives raw numeric enum values (e.g. 32759).
    static void packAttrInfo(const Octane::ApiAttributeInfo& info, octaneapi::ApiAttributeInfo* out) {
        out->set_id(static_cast<octaneapi::AttributeId>(info.mId));
        int rawType = static_cast<int>(info.mType);
        if (octaneapi::AttributeType_IsValid(rawType)) {
            out->set_type(static_cast<octaneapi::AttributeType>(rawType));
        } else {
            out->set_type(octaneapi::AT_UNKNOWN);
            ServerLog::instance().log("WRN", SVC, "packAttrInfo",
                "normalized unknown SDK type " + std::to_string(rawType) + " → AT_UNKNOWN");
        }
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
// ApiItemArrayService (apinodesystem_1) — array iteration
//
// Size and indexed access for ApiItemArrays returned by graph operations
// (getOwnedItems, findNodes, findItemsByName). Arrays are heap-allocated
// and owned by the handle registry — freed on Clear or UnregisterArray.
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
// ApiNodeService (apinodesystem_7) — node creation, connections, pin I/O
//
// Core scene-building API. Create nodes in a graph, connect/disconnect pins
// by name or index, read pin values (bool/int/float/float3). Pin children
// are auto-registered recursively on create so the client can immediately
// query attributes on auto-created child nodes (e.g. RT→camera→aperture).
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

            int rawType = static_cast<int>(request->type());
            if (!octaneapi::NodeType_IsValid(rawType) || rawType == 0) {
                return failInvalidArg(SVC, __func__,
                    "invalid node type " + std::to_string(rawType)
                    + " — must be a known NodeType (not NT_UNKNOWN/0)");
            }
            Octane::NodeType nodeType = static_cast<Octane::NodeType>(rawType);
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

            // Register all pin children recursively so the client can call
            // attrInfo/etc on them without first calling connectedNodeIx.
            // Pin children can have their own pin children (RT→camera→aperture→Float).
            if (configurePins) {
                registerPinChildrenRecursive(node);
            }
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
            response->set_result(safeNodeType(static_cast<int>(node->type()), "ApiNode::type"));
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
            response->set_result(safePinType(static_cast<int>(node->pinTypeIx(index)), "ApiNode::pinTypeIx"));
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

            // Post-connect verification: read back pin to confirm connection took effect.
            // SDK connectTo() is void — silent failure on type mismatch or invalid pin name.
            if (source) {
                Octane::ApiNode* actual = node->connectedNode(pinName.c_str(), false);
                if (!actual || actual->uniqueId() != source->uniqueId()) {
                    std::ostringstream oss;
                    oss << "connection to pin \"" << pinName << "\" on node " << request->objectptr().handle()
                        << " did not take effect — source " << request->sourcenode().handle()
                        << " may be incompatible with this pin type. Check pin type compatibility.";
                    ServerLog::instance().log("WRN", SVC, __func__, oss.str());
                }
            }
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

            uint32_t pinIdx = request->pinidx();
            // Don't bounds-check pinIdx — SDK handles it. Mesh nodes may have
            // pins before pinCount() reflects them (e.g. material slot on empty mesh).
            node->connectToIx(pinIdx, source, request->evaluate(), request->docyclecheck());

            // Post-connect verification: confirm connection took effect
            if (source && pinIdx < node->pinCount()) {
                Octane::ApiNode* actual = node->connectedNodeIx(pinIdx, false);
                if (!actual || actual->uniqueId() != source->uniqueId()) {
                    std::ostringstream oss;
                    oss << "connection to pin index " << pinIdx << " on node " << request->objectptr().handle()
                        << " did not take effect — source " << request->sourcenode().handle()
                        << " may be incompatible with this pin type. Check pin type compatibility.";
                    ServerLog::instance().log("WRN", SVC, __func__, oss.str());
                }
            }
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
            if (pinIx >= node->pinCount()) {
                std::ostringstream oss;
                oss << "pin index " << pinIx << " out of range (pinCount is " << node->pinCount() << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
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

            uint32_t pinIx = request->pinix();
            if (pinIx >= node->pinCount()) {
                std::ostringstream oss;
                oss << "pin index " << pinIx << " out of range (pinCount is " << node->pinCount() << ")";
                return failInvalidArg(SVC, __func__, oss.str());
            }
            Octane::ApiItem* owned = node->ownedItemIx(pinIx);
            if (owned) {
                uint64_t handle = sHandleRegistry->Register(owned);
                response->mutable_result()->set_handle(handle);
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Node info (instance-level, returns ApiNodeInfo for this node's type) ──
    grpc::Status info(grpc::ServerContext*, const octaneapi::ApiNode::infoRequest* request,
        octaneapi::ApiNode::infoResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;
            const Octane::ApiNodeInfo& ni = node->info();
            auto* r = response->mutable_result();
            r->set_type(safeNodeType(static_cast<int>(ni.mType), "ApiNode::info"));
            r->set_description(ni.mDescription ? ni.mDescription : "");
            r->set_outtype(safePinType(static_cast<int>(ni.mOutType), "ApiNode::info"));
            r->set_nodecolor(ni.mNodeColor);
            r->set_islinker(ni.mIsLinker);
            r->set_isoutputlinker(ni.mIsOutputLinker);
            r->set_takespindefaultvalue(ni.mTakesPinDefaultValue);
            r->set_ishidden(ni.mIsHidden);
            r->set_iscreatablebyapi(ni.mIsCreatableByApi);
            r->set_isscriptgraphwrapper(ni.mIsScriptGraphWrapper);
            r->set_istypedtexturenode(ni.mIsTypedTextureNode);
            r->set_category(ni.mCategory ? ni.mCategory : "");
            r->set_defaultname(ni.mDefaultName ? ni.mDefaultName : "");
            r->set_attributeinfocount(ni.mAttributeInfoCount);
            r->set_pininfocount(ni.mPinInfoCount);
            r->set_movableinputcountattribute(static_cast<octaneapi::AttributeId>(ni.mMovableInputCountAttribute));
            r->set_movableinputpincount(ni.mMovableInputPinCount);
            r->set_movableinputformat(static_cast<octaneapi::MovableInputFormat>(ni.mMovableInputFormat));
            r->set_movableinputname(ni.mMovableInputName ? ni.mMovableInputName : "");
            r->set_minversion(ni.mMinVersion);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Pin info by index (returns encoded node+pin handle, no cache) ──
    grpc::Status pinInfoIx(grpc::ServerContext*, const octaneapi::ApiNode::pinInfoIxRequest* request,
        octaneapi::ApiNode::pinInfoIxResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;
            uint32_t index = request->index();
            if (index >= node->pinCount()) {
                return grpc::Status::OK; // out of range = null handle
            }
            uint64_t handle = encodePinInfoHandle(request->objectptr().handle(), index);
            response->mutable_result()->set_handle(handle);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    // ── Get pin value by PinId ────────────────────────────────────────
    grpc::Status getPinValueByPinID(grpc::ServerContext*, const octaneapi::ApiNode::getPinValueByIDRequest* request,
        octaneapi::ApiNode::getPinValueByXResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            grpc::Status status;
            auto* node = requireNode(request->objectptr().handle(), SVC, __func__, status);
            if (!node) return status;

            Octane::PinId pinId = static_cast<Octane::PinId>(request->pin_id());
            // Determine pin type to dispatch correctly
            Octane::NodePinType pinType = node->pinType(pinId);

            switch (pinType) {
                case Octane::PT_BOOL: {
                    bool v = false; node->getPinValue(pinId, v);
                    response->set_bool_value(v);
                    break;
                }
                case Octane::PT_INT: {
                    int32_t v = 0; node->getPinValue(pinId, v);
                    response->set_int_value(v);
                    break;
                }
                case Octane::PT_FLOAT: {
                    float v = 0; node->getPinValue(pinId, v);
                    response->set_float_value(v);
                    break;
                }
                default: {
                    // For texture/geometry/material pins, try float3 (common for color pins)
                    Octane::float_3 v; v.x = 0; v.y = 0; v.z = 0;
                    node->getPinValue(pinId, v);
                    auto* r = response->mutable_float3_value();
                    r->set_x(v.x); r->set_y(v.y); r->set_z(v.z);
                    break;
                }
            }
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiNodeGraphService (apinodesystem_6) — scene tree traversal
//
// Graph-level queries: enumerate owned items, search by name or type,
// deep-copy item trees. Results are returned as ApiItemArrays managed
// by the handle registry. Used by MCP's get_scene_tree and find_nodes.
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
            response->set_result(safeGraphType(static_cast<int>(graph->type()), "NodeGraph::type"));
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ApiNodePinInfoExService — detailed pin metadata
//
// Returns full ApiNodePinInfo for a pin: type, name, label, description,
// float/int ranges with slider bounds, and enum value/label pairs. Uses
// encoded handles (high 48 bits = node uniqueId, low 16 bits = pin index)
// to avoid caching pin info objects server-side.
// ═══════════════════════════════════════════════════════════════════════════
class NodePinInfoExServiceImpl final : public octaneapi::ApiNodePinInfoExService::Service {
    static constexpr const char* SVC = "ApiNodePinInfoExService";

    // Helper to pack SDK ApiNodePinInfo into proto message.
    // Packs core fields; sub-info (float ranges, enum values) filled where available.
    static void packPinInfo(const Octane::ApiNodePinInfo& pi, octaneapi::ApiNodePinInfo* out) {
        out->set_id(static_cast<octaneapi::PinId>(pi.mId));
        out->set_type(safePinType(static_cast<int>(pi.mType), "packPinInfo"));
        out->set_istypedtexturepin(pi.mIsTypedTexturePin);
        out->set_staticname(pi.mStaticName ? pi.mStaticName : "");
        out->set_staticlabel(pi.mStaticLabel ? pi.mStaticLabel : "");
        out->set_description(pi.mDescription ? pi.mDescription : "");
        out->set_groupname(pi.mGroupName ? pi.mGroupName : "");
        out->set_pincolor(pi.mPinColor);
        out->set_defaultnodetype(safeNodeType(static_cast<int>(pi.mDefaultNodeType), "packPinInfo"));
        out->set_minversion(pi.mMinVersion);
        out->set_endversion(pi.mEndVersion);
        // Float pin info — dimension-based ranges
        if (pi.mFloatInfo) {
            auto* fi = out->mutable_floatinfo();
            fi->set_isvalid(true);
            fi->set_dimcount(pi.mFloatInfo->mDimCount);
            fi->set_usesliders(pi.mFloatInfo->mUseSliders);
            fi->set_allowlog(pi.mFloatInfo->mAllowLog);
            fi->set_defaultislog(pi.mFloatInfo->mDefaultIsLog);
            for (uint32_t d = 0; d < pi.mFloatInfo->mDimCount && d < 4; ++d) {
                auto* di = fi->add_diminfos();
                di->set_name(pi.mFloatInfo->mDimInfos[d].mName ? pi.mFloatInfo->mDimInfos[d].mName : "");
                di->set_minvalue(pi.mFloatInfo->mDimInfos[d].mMinValue);
                di->set_maxvalue(pi.mFloatInfo->mDimInfos[d].mMaxValue);
                di->set_sliderminvalue(pi.mFloatInfo->mDimInfos[d].mSliderMinValue);
                di->set_slidermaxvalue(pi.mFloatInfo->mDimInfos[d].mSliderMaxValue);
                di->set_sliderstep(pi.mFloatInfo->mDimInfos[d].mSliderStep);
            }
        }
        // Int pin info — dimension-based ranges
        if (pi.mIntInfo) {
            auto* ii = out->mutable_intinfo();
            ii->set_isvalid(true);
            ii->set_dimcount(pi.mIntInfo->mDimCount);
            for (uint32_t d = 0; d < pi.mIntInfo->mDimCount && d < 4; ++d) {
                auto* di = ii->add_diminfos();
                di->set_name(pi.mIntInfo->mDimInfos[d].mName ? pi.mIntInfo->mDimInfos[d].mName : "");
                di->set_minvalue(pi.mIntInfo->mDimInfos[d].mMinValue);
                di->set_maxvalue(pi.mIntInfo->mDimInfos[d].mMaxValue);
                di->set_sliderminvalue(pi.mIntInfo->mDimInfos[d].mSliderMinValue);
                di->set_slidermaxvalue(pi.mIntInfo->mDimInfos[d].mSliderMaxValue);
                di->set_sliderstep(pi.mIntInfo->mDimInfos[d].mSliderStep);
            }
        }
        // Enum pin info — value/label pairs
        if (pi.mEnumInfo) {
            auto* ei = out->mutable_enuminfo();
            ei->set_isvalid(true);
            ei->set_valuecount(pi.mEnumInfo->mValueCount);
            ei->set_defaultvalue(pi.mEnumInfo->mDefaultValue);
            for (uint32_t i = 0; i < pi.mEnumInfo->mValueCount; ++i) {
                auto* v = ei->add_values();
                v->set_value(pi.mEnumInfo->mValues[i].mValue);
                v->set_label(pi.mEnumInfo->mValues[i].mLabel ? pi.mEnumInfo->mValues[i].mLabel : "");
            }
        }
    }

public:
    grpc::Status getApiNodePinInfo(grpc::ServerContext*,
        const octaneapi::ApiNodePinInfoEx::GetNodePinInfoRequest* request,
        octaneapi::ApiNodePinInfoEx::GetNodePinInfoResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
            uint64_t encodedHandle = request->objectptr().handle();
            uint64_t nodeHandle = 0;
            uint32_t pinIndex = 0;
            decodePinInfoHandle(encodedHandle, nodeHandle, pinIndex);

            grpc::Status status;
            auto* node = requireNode(nodeHandle, SVC, __func__, status);
            if (!node) {
                response->set_success(false);
                return grpc::Status::OK;
            }
            if (pinIndex >= node->pinCount()) {
                response->set_success(false);
                return grpc::Status::OK;
            }
            const Octane::ApiNodePinInfo& pi = node->pinInfoIx(pinIndex);
            packPinInfo(pi, response->mutable_nodepininfo());
            response->set_success(true);
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// SharedSurfaceFrameService — dxSS viewport path
// ═══════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
// State for cross-process handle duplication
static std::atomic<DWORD> sViewportClientPid{0};
static std::mutex sSharedFrameMutex;
struct ClonedSurfaceEntry {
    Octane::ApiSharedSurface* surface;
    std::chrono::steady_clock::time_point created;
};
static std::map<uint64_t, ClonedSurfaceEntry> sClonedSurfaces;
static std::atomic<uint64_t> sNextFrameId{1};
static constexpr auto SS_CLONE_TTL = std::chrono::seconds(30);

// Purge cloned surfaces older than TTL (call under sSharedFrameMutex)
static void purgeStaleClones() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = sClonedSurfaces.begin(); it != sClonedSurfaces.end(); ) {
        if (now - it->second.created > SS_CLONE_TTL) {
            ServerLog::instance().log("WRN", "SharedSurfaceFrame", "purgeStaleClones",
                "purging stale clone frameId=" + std::to_string(it->first));
            it->second.surface->release();
            it = sClonedSurfaces.erase(it);
        } else {
            ++it;
        }
    }
}
#endif

class SharedSurfaceFrameServiceImpl final
    : public octaneapi::SharedSurfaceFrameService::Service {
    static constexpr const char* SVC = "SharedSurfaceFrame";

public:
    grpc::Status registerViewportClient(
        grpc::ServerContext*,
        const octaneapi::RegisterViewportClientRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
#ifdef _WIN32
            DWORD pid = request->pid();
            sViewportClientPid.store(pid);
            ServerLog::instance().res(SVC, __func__,
                "Registered viewport client PID " + std::to_string(pid));
#endif
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status grabSharedFrame(
        grpc::ServerContext*,
        const google::protobuf::Empty*,
        octaneapi::GrabSharedFrameResponse* response) override {
        GRPC_SAFE_BEGIN(SVC)
#ifdef _WIN32
            DWORD clientPid = sViewportClientPid.load();
            if (clientPid == 0) {
                response->set_result(false);
                return grpc::Status::OK;
            }

            // Grab the current render result
            Octane::ApiArray<Octane::ApiRenderImage> renderImages;
            bool grabbed = Octane::ApiRenderEngine::grabRenderResult(renderImages);
            if (!grabbed || renderImages.mSize == 0) {
                response->set_result(false);
                return grpc::Status::OK;
            }

            // Find first image with a shared surface
            const Octane::ApiRenderImage* ssImage = nullptr;
            for (size_t i = 0; i < renderImages.mSize; ++i) {
                if (renderImages.mData[i].mSharedSurface) {
                    ssImage = &renderImages.mData[i];
                    break;
                }
            }

            if (!ssImage) {
                // No shared surface — caller should fall back to pixel path.
                // Release the render result since we're not using the pixels here.
                Octane::ApiRenderEngine::releaseRenderResult();
                response->set_result(false);
                return grpc::Status::OK;
            }

            // Clone the shared surface to keep it alive after releaseRenderResult
            Octane::ApiSharedSurface* cloned = ssImage->mSharedSurface->clone();
            if (!cloned) {
                Octane::ApiRenderEngine::releaseRenderResult();
                response->set_result(false);
                ServerLog::instance().err(SVC, __func__, "shared surface clone() returned null");
                return grpc::Status(grpc::StatusCode::INTERNAL, "shared surface clone failed");
            }
            void* ssHandle = cloned->getD3D11Handle();
            uint64_t luid  = cloned->getD3D11AdapterLuid();

            // Release the render result (cloned surface stays alive)
            Octane::ApiRenderEngine::releaseRenderResult();

            // DuplicateHandle into the Electron process
            HANDLE targetProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, clientPid);
            if (!targetProc) {
                cloned->release();
                response->set_result(false);
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    "Failed to open client process for DuplicateHandle");
            }

            HANDLE dupHandle = NULL;
            BOOL dupOk = DuplicateHandle(
                GetCurrentProcess(), ssHandle,
                targetProc, &dupHandle,
                0, FALSE, DUPLICATE_SAME_ACCESS);
            CloseHandle(targetProc);

            if (!dupOk) {
                cloned->release();
                response->set_result(false);
                return grpc::Status(grpc::StatusCode::INTERNAL,
                    "DuplicateHandle failed");
            }

            // Track the cloned surface for later release
            uint64_t frameId = sNextFrameId.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(sSharedFrameMutex);
                purgeStaleClones();
                sClonedSurfaces[frameId] = { cloned, std::chrono::steady_clock::now() };
            }

            // Get dimensions — ApiRenderImage.mSize may be {0,0} when SS is active,
            // so fall back to render statistics if needed.
            uint32_t frameW = ssImage->mSize.x;
            uint32_t frameH = ssImage->mSize.y;
            uint32_t framePitch = ssImage->mPitch;
            auto frameType = ssImage->mType;

            if (frameW == 0 || frameH == 0) {
                // SS mode: mSize is not populated — get from render statistics
                Octane::RenderResultStatistics stats;
                stats.clear();
                Octane::ApiRenderEngine::getRenderStatistics(stats);
                if (stats.mSetSize.x > 0 && stats.mSetSize.y > 0) {
                    frameW = stats.mSetSize.x;
                    frameH = stats.mSetSize.y;
                    // Estimate pitch: width * bytes_per_pixel
                    uint32_t bpp = (frameType == Octane::IMAGE_TYPE_HDR_RGBA) ? 16 : 4;
                    framePitch = frameW * bpp;
                    // Default to LDR if type is 0
                    if (static_cast<int>(frameType) == 0) {
                        frameType = Octane::IMAGE_TYPE_LDR_RGBA;
                    }
                }
            }

            ServerLog::instance().res(SVC, "grabSharedFrame",
                "frame=" + std::to_string(frameW) + "x" + std::to_string(frameH)
                + " pitch=" + std::to_string(framePitch)
                + " type=" + std::to_string(static_cast<int>(frameType))
                + " frameId=" + std::to_string(frameId));

            // Populate the response
            auto* frame = response->mutable_frame();
            frame->set_handle(reinterpret_cast<uint64_t>(dupHandle));
            frame->set_adapterluid(luid);
            frame->set_width(frameW);
            frame->set_height(frameH);
            frame->set_pitch(framePitch);
            frame->set_format(static_cast<uint32_t>(frameType == Octane::IMAGE_TYPE_HDR_RGBA
                ? 2   // DXGI_FORMAT_R32G32B32A32_FLOAT
                : 28  // DXGI_FORMAT_R8G8B8A8_UNORM
            ));
            frame->set_frameid(frameId);
            frame->set_samplesperpixel(ssImage->mTonemappedSamplesPerPixel);
            frame->set_rendertime(ssImage->mRenderTime);
            frame->set_imagetype(static_cast<uint32_t>(ssImage->mType));
            response->set_result(true);
#else
            response->set_result(false);
#endif
            return grpc::Status::OK;
        GRPC_SAFE_END(SVC)
    }

    grpc::Status releaseSharedFrame(
        grpc::ServerContext*,
        const octaneapi::ReleaseSharedFrameRequest* request,
        google::protobuf::Empty*) override {
        GRPC_SAFE_BEGIN(SVC)
#ifdef _WIN32
            uint64_t frameId = request->frameid();
            std::lock_guard<std::mutex> lock(sSharedFrameMutex);
            auto it = sClonedSurfaces.find(frameId);
            if (it != sClonedSurfaces.end()) {
                it->second.surface->release();
                sClonedSurfaces.erase(it);
            } else {
                ServerLog::instance().log("WRN", SVC, __func__,
                    "frameId " + std::to_string(frameId) + " not found in cloned surfaces (already released or never cloned)");
            }
#endif
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
    std::string serverAddress = "127.0.0.1:" + std::to_string(mPort);

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
    NodePinInfoExServiceImpl      nodePinInfoExService;
    SharedSurfaceFrameServiceImpl sharedSurfaceFrameService;

    // Shared surface output is NOT enabled at startup — it must be enabled
    // on demand by the Electron client via the setSharedSurfaceOutputType RPC.
    // Enabling it globally breaks the regular grabRenderResult pixel path
    // (mBuffer becomes null when the SDK produces shared surfaces instead).

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
    builder.RegisterService(&nodePinInfoExService);
    builder.RegisterService(&sharedSurfaceFrameService);

    // 64 MB — matches Octane's built-in gRPC server message size limit
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);

    // Allow frequent client keepalive pings (octaneWebR sends every 10-20s).
    // Default gRPC server rejects pings more frequent than 5 minutes.
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

    mServer = builder.BuildAndStart();
    if (!mServer) {
        std::string detail = "Failed to start gRPC server on " + serverAddress
            + " — port " + std::to_string(mPort) + " is likely in use by another process."
            + " Kill the other octaneServGrpc instance (taskkill //F //IM octaneServGrpc.exe)"
            + " or run on a different port: octaneServGrpc.exe " + std::to_string(mPort + 1);
        std::cerr << "[OctaneServGrpc] ERROR: " << detail << std::endl;
        ServerLog::instance().err("GrpcServer", "RunServer", detail);
        return;
    }

    mRunning = true;
    API_RLOG(serv, "gRPC server listening on %s", serverAddress.c_str());
    std::cout << "[OctaneServGrpc] gRPC server listening on " << serverAddress << std::endl;
    std::cout << "[OctaneServGrpc] Services: ApiInfo, ProjectManager, ChangeManager, RenderEngine, LiveLink, StreamCallback, Item, ItemArray, Node, NodeGraph" << std::endl;
    std::cout << "[OctaneServGrpc] Hardening: GRPC_SAFE on all RPCs, validated lookups, input bounds, SEH protection" << std::endl;

    // Block until StopServer() is called
    mServer->Wait();
    mRunning = false;

    API_RLOG(serv, "gRPC server stopped");
    std::cout << "[OctaneServGrpc] gRPC server stopped." << std::endl;
}

void GrpcServer::StopServer() {
    if (mServer) {
        mServer->Shutdown();
    }
}

} // namespace OctaneServ
