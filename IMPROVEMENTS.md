# Improvements & Backlog

## Phase 2 — Core Functionality (Next)

### P0: Render Streaming
- [ ] Wire `StreamCallbackService.callbackChannel()` to SDK callbacks
- [ ] Register `ApiRenderEngine::setOnNewImageCallback()` when client connects
- [ ] Convert `ApiRenderImage` → proto `OnNewImageRequest` → `stream->Write()`
- [ ] Forward `OnNewStatistics`, `OnRenderFailure`, `ProjectManagerChanged`
- [ ] Unregister callbacks on client disconnect
- [ ] Test: octaneWebR viewport shows live render frames

### P0: Node Service
- [ ] `ApiNode::create(nodeType, rootGraph, autoEvaluate)` — create nodes
- [ ] `connectTo(pin, source, evaluate, relink)` — connect pins
- [ ] `connectToIx(index, source, evaluate, relink)` — connect by index
- [ ] `setPinValue()` — set float3/int/bool/string pin values
- [ ] `getPinValue()` — read pin values
- [ ] `staticPinCount()`, `pinCount()` — pin enumeration
- [ ] `destroy()` — delete nodes
- [ ] Test: MCP can build scenes (create RT, camera, kernel, geo, materials)

### P0: Item Service (Attributes)
- [ ] `getByAttrID(handle, attrId, expectedType)` — read attributes
- [ ] `setByAttrID(handle, attrId, expectedType, value)` — write attributes
- [ ] `hasAttr(handle, attrId)` — check attribute existence
- [ ] `attrCount()`, `attrId()`, `attrType()` — attribute enumeration
- [ ] Test: MCP can set translations, rotations, filenames, values

## Phase 3 — Full Compatibility

### P1: Render Engine (remaining methods)
- [ ] `getRenderStatistics()` — sample count, render time
- [ ] `saveImage()` / `saveImage1()` / `saveImage2()` — save renders to disk
- [ ] `saveRenderPasses()` — multi-pass export
- [ ] `saveRenderPassesMultiExr()` — EXR export
- [ ] `getEnabledAovs()` — list active render passes
- [ ] `isCompiling()`, `isCompressingTextures()`, `hasPendingRenderData()` — render state

### P1: Project Manager (remaining)
- [ ] `saveProjectAs()` with reference package
- [ ] `loadedFromPackage()`, `loadedOcsVersion()`
- [ ] `addObserver()`, `removeObserver()` — project change notifications

### P1: Scene Outliner
- [ ] `ApiSceneOutliner` service — if octaneWebR uses it

### P2: File Chooser
- [ ] `ApiFileChooser` — file browser for ORBX/OBJ selection

### P2: LiveDB / LocalDB
- [ ] `ApiDBMaterialManager` — online material database
- [ ] `ApiLocalDB` — local material packages

### P2: MaterialX
- [ ] `ApiMaterialXGlobal` — import .mtlx files

### P2: OCIO
- [ ] `ApiOcioConfig`, `ApiOcioConfigLoader`, `ApiOcioContextManager`

## Phase 4 — Production Hardening

- [ ] Input validation on all handle lookups (return INVALID_ARGUMENT, not crash)
- [ ] Graceful handling of SDK exceptions (catch, log, return gRPC error)
- [ ] Connection tracking (log client connect/disconnect)
- [ ] Health check endpoint (gRPC health check protocol)
- [ ] Metrics: RPC count, latency histogram, error rate
- [ ] Config file for port, log level, SDK path (instead of CLI args)
- [ ] Installer / NSIS package (like octaneWebR's Electron build)
- [ ] CI build (GitHub Actions or similar)

## Phase 5 — Advanced

- [ ] Multiple simultaneous clients (verify thread safety)
- [ ] Scene state mirroring (bridge maintains its own scene graph for queries)
- [ ] Batch RPC support (multiple operations in one call)
- [ ] gRPC reflection service (for grpcurl / debugging)
- [ ] TLS support (secure connections)
- [ ] Authentication (API keys for client access)
- [ ] Alias layer for gRPC: maps Octane C++ SDK style method/field names to Octane Lua API style names — both work. Enables clients to call using either convention.
