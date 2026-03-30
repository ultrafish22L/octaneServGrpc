# Reference

## gRPC Services → SDK Mapping

### ApiInfoService (`apiinfo.proto`)
| gRPC Method | SDK Call | Returns |
|---|---|---|
| `octaneVersion` | `Octane::ApiInfo::octaneVersion()` | uint32 |
| `octaneName` | `Octane::ApiInfo::octaneName()` | string |
| `isDemoVersion` | `Octane::ApiInfo::isDemoVersion()` | bool |
| `isSubscriptionVersion` | `Octane::ApiInfo::isSubscriptionVersion()` | bool |
| `tierIdx` | `Octane::ApiInfo::tierIdx()` | uint32 |

### ApiProjectManagerService (`apiprojectmanager.proto`)
| gRPC Method | SDK Call | Returns |
|---|---|---|
| `rootNodeGraph` | `ApiProjectManager::rootNodeGraph()` | ObjectRef |
| `resetProject` | `ApiProjectManager::resetProject()` | bool |
| `loadProject(path, evaluate)` | `ApiProjectManager::loadProject(path, nullptr, 0, evaluate)` | bool |
| `saveProject` | `ApiProjectManager::saveProject()` | bool |
| `saveProjectAs(path)` | `ApiProjectManager::saveProjectAs(path)` | bool |
| `getCurrentProject` | `ApiProjectManager::getCurrentProject()` | string |

### ApiChangeManagerService (`apichangemanager.proto`)
| gRPC Method | SDK Call |
|---|---|
| `update` | `ApiChangeManager::update()` |

### ApiRenderEngineService (`apirender.proto`)
| gRPC Method | SDK Call | Returns |
|---|---|---|
| `getDeviceCount` | `ApiRenderEngine::getDeviceCount()` | uint32 |
| `getDeviceName(index)` | `ApiRenderEngine::getDeviceName(index)` | string |
| `clayMode` | `ApiRenderEngine::clayMode()` | enum |
| `setClayMode(mode)` | `ApiRenderEngine::setClayMode(mode)` | void |
| `getSubSampleMode` | `ApiRenderEngine::getSubSampleMode()` | enum |
| `setSubSampleMode(mode)` | `ApiRenderEngine::setSubSampleMode(mode)` | void |
| `renderPriority` | `ApiRenderEngine::renderPriority()` | enum |
| `setRenderPriority(prio)` | `ApiRenderEngine::setRenderPriority(prio)` | void |
| `setRenderTargetNode(node)` | `ApiRenderEngine::setRenderTargetNode(node)` | bool |
| `getRenderTargetNode` | `ApiRenderEngine::getRenderTargetNode()` | ObjectRef |
| `stopRendering` | `ApiRenderEngine::stopRendering()` | void |
| `continueRendering` | `ApiRenderEngine::continueRendering()` | void |
| `restartRendering` | `ApiRenderEngine::restartRendering()` | void |
| `pauseRendering` | `ApiRenderEngine::pauseRendering()` | void |
| `isRenderingPaused` | `ApiRenderEngine::isRenderingPaused()` | bool |
| `getMemoryUsage(deviceIx)` | `ApiRenderEngine::getMemoryUsage(ix, memUsage)` | ApiDeviceMemoryUsage |
| `getSceneBounds` | `ApiRenderEngine::getSceneBounds(bmin, bmax)` | bool + float3×2 |
| `getRealTime` | `ApiRenderEngine::getRealTime()` | bool |

### LiveLinkService (`livelink.proto`)
| gRPC Method | SDK Call |
|---|---|
| `GetCamera` | Read P_POSITION/P_TARGET/P_UP from RT camera node |
| `SetCamera(pos, target, up)` | Write P_POSITION/P_TARGET/P_UP + `ApiChangeManager::update()` |

### ApiItemService (`apinodesystem_3.proto`)
| gRPC Method | SDK Call | Returns |
|---|---|---|
| `isGraph(handle)` | `item->isGraph()` | bool |
| `isNode(handle)` | `item->isNode()` | bool |
| `name(handle)` | `item->name()` | string |
| `setName(handle, name)` | `item->setName(name)` | void |
| `outType(handle)` | `item->outType()` | enum |
| `uniqueId(handle)` | `item->uniqueId()` | uint32 |
| `destroy(handle)` | `item->destroy()` | void |
| `evaluate(handle)` | `item->evaluate()` | void |

### ApiItemArrayService (`apinodesystem_1.proto`)
| gRPC Method | SDK Call | Returns |
|---|---|---|
| `size(handle)` | `arr->size()` | uint32 |
| `get(handle, index)` | `arr->get(index)` | ObjectRef |

### ApiNodeGraphService (`apinodesystem_6.proto`)
| gRPC Method | SDK Call | Returns |
|---|---|---|
| `getOwnedItems(handle)` | `graph->getOwnedItems(itemArray)` | ObjectRef (array handle) |
| `type1(handle)` | `item->outType()` | enum |

## Handle Types

| Proto Field | C++ Type | Registry Method |
|---|---|---|
| `ObjectRef objectPtr` | `ApiItem*` | `Lookup(handle)` |
| `ObjectRef list` (from getOwnedItems) | `ApiItemArray*` | `LookupArray(handle)` |
| `ObjectRef result` (from create/get) | `ApiItem*` | `Register(item)` |
| `ObjectRef targetNode` (setRenderTargetNode) | `ApiNode*` | `Lookup(handle)` → cast |

## SDK Constants Used

| Constant | Value | Used In |
|---|---|---|
| `Octane::P_CAMERA` | pin ID | LiveLink (get camera from RT) |
| `Octane::P_POSITION` | pin ID | LiveLink (camera position) |
| `Octane::P_TARGET` | pin ID | LiveLink (camera target) |
| `Octane::P_UP` | pin ID | LiveLink (camera up vector) |
| `Octane::P_MESH` | pin ID | RenderEngine (get geometry node) |

## Log Levels

Set via `--log-level=<level>` or `SERV_LOG_LEVEL` env var.

| Level | File (`log_serv.log`) | Octane Log Window |
|---|---|---|
| `verbose` | ALL RPC calls (firehose) | ALL RPC calls |
| `debug` (DEFAULT) | Mutating + lifecycle + curated reads | Mutating + lifecycle + curated reads |
| `info` | Mutating + lifecycle + errors only | Mutating + lifecycle + errors |
| `warn` | Errors only | Errors only |
| `off` | Disabled | Mutating + lifecycle + errors (always forwarded) |

Mutating methods: create, destroy, setByAttrID, connectTo, startRendering, stopRendering, SetCamera, update, rootNodeGraph, etc.

Debug methods: GetCamera, hasAttr, getPinValue, getRenderTargetNode, getDeviceName, getDeviceCount, getMemoryUsage, getRealTime, clayMode, renderPriority, octaneVersion, etc.
