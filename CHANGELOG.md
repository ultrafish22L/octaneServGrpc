# Changelog

## v1.0.0 — 2026-03-22

### Initial Release

**Project scaffolding:**
- CMakeLists.txt with FetchContent gRPC (v1.62.1, auto-download)
- 96 Beta 2 proto files compiled to C++ server stubs
- Link Octane Render SDK 2026.2 (`octane.lib` + `octane.dll`)
- Post-build copies all Octane runtime DLLs to output dir
- Windows tray app mode (from octaneservermodule) + console debug mode

**SDK integration:**
- `SdkEngine` — init, auth (HMAC-SHA256 via bcrypt), activate, exit
- PLORTEST plugin keys (T0/T1/T2) from SDK key files
- Version/tier validation on startup

**Services implemented:**
- `ApiInfoService` — octaneVersion, octaneName, isDemoVersion, isSubscriptionVersion, tierIdx
- `ApiProjectManagerService` — rootNodeGraph, loadProject, saveProject, resetProject, getCurrentProject
- `ApiChangeManagerService` — update (scene evaluation)
- `ApiRenderEngineService` — getDeviceCount/Name, clayMode, subSampleMode, renderPriority, setRenderTargetNode, getRenderTargetNode, stopRendering, continueRendering, restartRendering, pauseRendering, getMemoryUsage, getSceneBounds, getRealTime
- `LiveLinkService` — GetCamera, SetCamera (reads/writes P_POSITION/P_TARGET/P_UP on camera node)
- `StreamCallbackServiceImpl` — callbackChannel (stream alive, callbacks not yet wired)
- `ApiItemService` — isGraph, isNode, name, setName, outType, uniqueId, destroy, evaluate, deleteUnconnectedItems
- `ApiItemArrayService` — size, get
- `ApiNodeGraphService` — getOwnedItems, type1

**Infrastructure:**
- HandleRegistry — maps gRPC uint64 handles to SDK ApiItem*/ApiItemArray*
- ServerLog — level-based file logging (verbose/debug/info/warn/off)
- LoggingInterceptor — gRPC interceptor, auto-logs every RPC call

**Rename (v1.0.0-rc2):**
- Moved from `C:\otoyla\GRPC\octane-grpc-bridge\` to `C:\otoyla\GRPC\dev\octaneServGrpc\`
- Renamed exe from `octane-grpc-bridge.exe` to `octaneServGrpc.exe`
- Renamed all "bridge/Bridge/BRIDGE" references to "OctaneServ" throughout codebase

**Verified:**
- Connects to octaneWebR — shows "OctaneRender Studio+ 2026.2 | Subscription | Tier 2"
- MCP `get_device_info` returns RTX 4090 / 24GB VRAM
- MCP `load_project` loads teapot.orbx successfully (18ms on server side)
- Scene tree traversal works (getOwnedItems → size → get)
- Zero bridge-side errors on scene load
