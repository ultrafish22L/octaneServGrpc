# octaneServGrpc

## Current Session

**Phase 1: Project scaffolding, rename, core services**

**What exists:**
- C++ gRPC server linking Octane Render SDK 2026.2 (`octane.dll`/`octane.lib`)
- 9 gRPC services registered, serving 96 Beta 2 protos on port 51022
- SDK lifecycle: init, auth (PLORTEST/HMAC-SHA256), activate, shutdown
- Windows tray app + console mode (lifted from octaneservermodule)
- Level-based file logging (`log_serv.log`) with gRPC interceptor
- Handle registry mapping gRPC uint64 handles to SDK `ApiItem*` / `ApiItemArray*`
- Successfully tested with octaneWebR: connects, shows version, device info, loads ORBX

**What's incomplete:**
- `StreamCallbackService.callbackChannel()` — stub, no SDK callbacks wired (no live render)
- `NodeServiceImpl` — placeholder, no create/connect/setPinValue
- `RenderEngine` callback registration — returns dummy IDs
- No `getRenderStatistics`, `saveImage` fully wired

### TODO for Next Session
1. **Wire StreamCallbackService** — register SDK `setOnNewImageCallback` → forward to gRPC stream
2. **Implement NodeService** — `create()`, `connectTo()`, `connectToIx()`, `setPinValue()`, `destroy()`
3. **Implement remaining ItemService methods** — `getByAttrID()`, `setByAttrID()`, `hasAttr()`
4. **Test teapot render end-to-end** — load ORBX, verify render frames stream to octaneWebR viewport
5. **Test MCP scene building** — create nodes, set attrs, connect, render via MCP tools

## Quick Start

| What | How |
|------|-----|
| Configure | `cd build && cmake .. -G "Visual Studio 17 2022"` (first time: ~4 min, downloads gRPC) |
| Build | `cmake --build . --config Release --target octaneServGrpc` |
| Run | `build/Release/octaneServGrpc.exe [port]` (default 51022) |
| Log level | `--log-level=debug` or `SERV_LOG_LEVEL=info` env var |
| Log file | `build/Release/log_serv.log` |
| Kill | `taskkill //F //IM octaneServGrpc.exe` |
| Test | Point octaneWebR at port 51022: `npm run dev` |

## Architecture

```
octaneWebR / MCP
    │  gRPC (Beta 2 protos, port 51022)
    ▼
┌─────────────────────────────────┐
│  octaneServGrpc.exe             │
│                                 │
│  Links: octane.dll (2026.2 SDK) │
│  Serves: 96 Beta 2 proto files  │
│                                 │
│  gRPC method → SDK C++ API call │
│  SDK callbacks → gRPC stream    │
└─────────────────────────────────┘
```

No separate octane.exe needed. The server IS the render engine.

## Project Structure

```
octaneServGrpc/
├── CMakeLists.txt           # Build: FetchContent gRPC, link octane.lib, compile protos
├── proto/                   # 96 Beta 2 proto files (from octaneWebR/server/proto/)
├── assets/install.ico       # Octane gear icon (tray app)
├── src/
│   ├── main.cpp             # Entry: console + Win32 tray app modes
│   ├── sdk_engine.h/cpp     # SDK lifecycle (init, auth, activate, exit)
│   ├── grpc_server.h/cpp    # gRPC server + all service implementations
│   ├── SERVKEY.h/cpp        # Plugin auth keys (PLORTEST)
│   ├── OctaneServGrpc.rc    # Windows resources (icon, menu, about dialog)
│   ├── resource.h           # Resource IDs
│   ├── targetver.h          # Windows SDK targeting
│   ├── services/
│   │   └── info_service.h   # (placeholder, services live in grpc_server.cpp)
│   └── util/
│       ├── handle_registry.h/cpp    # gRPC handles ↔ SDK pointers
│       ├── server_log.h/cpp         # Level-based file logging
│       └── logging_interceptor.h    # gRPC interceptor (auto REQ/RES/ERR logging)
└── build/Release/           # Output: exe + octane.dll + runtime DLLs (~911MB)
```

## Services Implemented

| Service | Proto | Status | Key Methods |
|---------|-------|--------|-------------|
| ApiInfo | apiinfo.proto | Done | octaneVersion, octaneName, isDemoVersion, tierIdx |
| ApiProjectManager | apiprojectmanager.proto | Done | rootNodeGraph, loadProject, saveProject, resetProject |
| ApiChangeManager | apichangemanager.proto | Done | update |
| ApiRenderEngine | apirender.proto | Partial | getDeviceCount/Name, clayMode, renderPriority, setRenderTargetNode, memory, bounds |
| LiveLink | livelink.proto | Done | GetCamera, SetCamera (reads/writes P_POSITION/P_TARGET/P_UP) |
| StreamCallbackService | callbackstream.proto | Stub | callbackChannel (keeps stream alive, no SDK callbacks wired) |
| ApiItem | apinodesystem_3.proto | Partial | isGraph, isNode, name, destroy, evaluate |
| ApiItemArray | apinodesystem_1.proto | Done | size, get |
| ApiNodeGraph | apinodesystem_6.proto | Partial | getOwnedItems, type1 |
| ApiNode | apinodesystem_7.proto | Stub | Placeholder, no methods |

## Key Patterns

**Handle Registry:** gRPC uses uint64 handles (`ObjectRef`). SDK uses C++ pointers.
- Items: handle = `ApiItem::uniqueId()` (stable, immutable)
- Arrays: synthetic handles starting at `0x8000000000000000` (heap-allocated, registry owns)
- Cleared on `resetProject()` / `loadProject()` to prevent stale pointers

**Service Implementation Pattern:**
```cpp
grpc::Status methodName(grpc::ServerContext*, const Request*, Response* response) override {
    auto* item = sHandleRegistry->Lookup(request->objectptr().handle());
    // Call SDK: Octane::ApiSomething::method(item, ...)
    // Pack result into response
    return grpc::Status::OK;
}
```

**Logging:** gRPC interceptor auto-logs every RPC. Levels: verbose > debug > info > warn > off.

## Dependencies

- **Octane SDK 2026.2**: `dev/OctaneRenderSDK_Studio+_2026_2_win/` (lib + headers + runtime)
- **gRPC C++ v1.62.1**: auto-downloaded via CMake FetchContent (includes protobuf, abseil, BoringSSL)
- **MSVC**: Visual Studio 2022, C++17, DLL CRT
- **Windows**: bcrypt.lib (HMAC auth), ws2_32.lib, crypt32.lib
