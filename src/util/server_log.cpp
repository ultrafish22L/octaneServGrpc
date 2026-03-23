#include "server_log.h"

namespace OctaneServ {

// Mutating + lifecycle methods — logged at info+
// Mirrors the client's GRPC_MUTATING_METHODS set
const std::unordered_set<std::string> ServerLog::sMutatingMethods = {
    "create", "destroy",
    "setByAttrID", "setValueByAttrID",
    "setByIx", "setValueByIx",
    "setByName", "setValueByName",
    "setPinValue", "setPinValueByPinID",
    "connectTo", "connectTo1", "connectToIx", "disconnectPin",
    "setPosition",
    "setRenderTargetNode",
    "continueRendering", "startRendering", "stopRendering",
    "restartRendering", "pauseRendering",
    "saveImage", "saveImage1", "saveImage2",
    "SetCamera",
    "update",           // ApiChangeManager.update (scene eval)
    "rootNodeGraph",    // project open
    "resetProject", "loadProject", "saveProject", "saveProjectAs",
    "setClayMode", "setSubSampleMode", "setRenderPriority",
};

// Methods shown at debug level (in addition to mutating methods above)
// Mirrors the client's GRPC_DEBUG_METHODS set
const std::unordered_set<std::string> ServerLog::sDebugMethods = {
    "GetCamera",
    "hasAttr",
    "getPinValue",
    "getRenderTargetNode",
    "getDeviceName", "getDeviceCount", "getMemoryUsage",
    "getRealTime",
    "clayMode", "renderPriority",
    "getSubSampleMode",
    "octaneVersion", "octaneName",
    "isDemoVersion", "isSubscriptionVersion", "tierIdx",
    "getSceneBounds",
    "isGraph", "isNode",
    "name", "outType", "uniqueId",
    "pinCount", "staticPinCount",
    "connectedNode",
    "callbackChannel",
};

} // namespace OctaneServ
