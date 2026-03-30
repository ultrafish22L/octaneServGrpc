# Architecture

## Overview

octaneServGrpc is a **thin gRPC wrapper** around the Octane Render SDK. It translates gRPC method calls into direct C++ API calls on the SDK, and forwards SDK callbacks back to clients via gRPC streams.

```
┌─────────────────────────────────────────────────────────────┐
│ Clients (unchanged)                                         │
│                                                             │
│  octaneWebR (Browser)         MCP Server                   │
│    │ HTTP → Node.js proxy       │ OctaneGrpcClientBase     │
│    └──────────┬─────────────────┘                          │
│               │ gRPC (96 Octane protos)                       │
└───────────────┼─────────────────────────────────────────────┘
                │ port 51022
┌───────────────▼─────────────────────────────────────────────┐
│ octaneServGrpc.exe                                          │
│                                                             │
│  ┌─────────────────────────────────────────────────┐       │
│  │ gRPC Server (grpc::ServerBuilder)               │       │
│  │                                                 │       │
│  │  InfoServiceImpl          ← ApiInfo SDK calls   │       │
│  │  ProjectManagerServiceImpl← ApiProjectManager   │       │
│  │  ChangeManagerServiceImpl ← ApiChangeManager    │       │
│  │  RenderEngineServiceImpl  ← ApiRenderEngine     │       │
│  │  LiveLinkServiceImpl      ← camera pin R/W      │       │
│  │  StreamCallbackServiceImpl← SDK event stream    │       │
│  │  ItemServiceImpl          ← ApiItem attrs       │       │
│  │  ItemArrayServiceImpl     ← array iteration     │       │
│  │  NodeServiceImpl          ← ApiNode CRUD        │       │
│  │  NodeGraphServiceImpl     ← scene tree walk     │       │
│  └─────────────────────────────────────────────────┘       │
│                        │                                    │
│  ┌─────────────────────▼───────────────────────────┐       │
│  │ Handle Registry                                 │       │
│  │  uint64 handle ↔ ApiItem* pointer               │       │
│  │  uint64 handle ↔ ApiItemArray* (owned)           │       │
│  └─────────────────────────────────────────────────┘       │
│                        │                                    │
│  ┌─────────────────────▼───────────────────────────┐       │
│  │ Octane SDK (octane.dll)                         │       │
│  │  ApiInfo, ApiNode, ApiNodeGraph, ApiItem,        │       │
│  │  ApiRenderEngine, ApiProjectManager,             │       │
│  │  ApiChangeManager, ...                           │       │
│  │                                                 │       │
│  │  GPU: RTX 4090 (Vulkan/CUDA)                    │       │
│  └─────────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────────┘
```

## Thread Model

- **Main thread**: Win32 message pump (tray icon) or console input loop
- **AppThread**: runs `GrpcServer::RunServer()` (blocks on `mServer->Wait()`)
- **gRPC thread pool**: handles concurrent client RPCs
- **SDK**: called from gRPC threads; SDK is internally thread-safe

## Handle Registry

The gRPC protos use `ObjectRef` (uint64 handles). The SDK uses C++ pointers.

| Handle Range | Type | Lifetime | Source |
|---|---|---|---|
| `0x00000001` – `0x7FFFFFFF` | ApiItem* | SDK-managed (scene nodes) | `ApiItem::uniqueId()` |
| `0x8000000000000000+` | ApiItemArray* | Registry-owned (heap) | Synthetic counter |

Cleared on `resetProject()` / `loadProject()` to prevent stale pointers.

## Logging

Three layers:
1. **gRPC Interceptor** (`logging_interceptor.h`) — auto-logs every RPC (service.method, elapsed ms, status). Also acts as the SDK readiness guard (rejects all RPCs with UNAVAILABLE before SDK init).
2. **ServerLog** (`server_log.h`) — level-filtered file logger with dual output:
   - **File**: `log_serv.log` next to exe (controlled by `--log-level`, default: `debug`)
   - **Octane log window**: via external sink (fires independently of file level — errors and mutating ops always forwarded, even with `--log-level=off`)
3. **SDK log flag** (`API_RLOG(serv, ...)`) — lifecycle events (init, shutdown) logged to Octane's own log system

Levels: `verbose` > `debug` (default) > `info` > `warn` > `off`

Format matches octaneWebR's `log_grpc.log` for side-by-side diff:
```
[HH:MM:SS.mmm]  REQ ApiInfoService.octaneVersion
[HH:MM:SS.mmm]  RES ApiInfoService.octaneVersion 0ms
[HH:MM:SS.mmm]  ERR ApiItemService.isGraph 0ms code=5 Not found
```

See `docs/TROUBLESHOOTING.md` § Debugging for the full log file table across both repos.

## SDK Lifecycle

```
1. apiMode_Shared_start("PLORTEST", authCallback, true)
2. apiMode_activate(&error, false)     // license check
3. [run gRPC server, handle RPCs]
4. apiMode_Shared_exit()               // shutdown
```

Auth uses HMAC-SHA256 via Windows CNG (`bcrypt.lib`). Keys in `SERVKEY.cpp` from PLORTEST plugin key files.

## Proto Compatibility

octaneServGrpc serves the **exact same protos** as octane.exe's built-in gRPC server. This means:
- octaneWebR's `OctaneGrpcClientBase` connects without changes
- MCP's gRPC client connects without changes
- No API version translation needed (pass-through mode)
- 96 proto files compiled to C++ server stubs via protoc
