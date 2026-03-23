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
│               │ gRPC (Beta 2 protos)                       │
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

Two layers:
1. **gRPC Interceptor** (`logging_interceptor.h`) — auto-logs every RPC (service.method, elapsed ms, status)
2. **ServerLog** (`server_log.h`) — level-filtered file logger

Levels: `verbose` > `debug` (default) > `info` > `warn` > `off`

Output: `log_serv.log` next to exe. Format matches octaneWebR's `log_grpc.log`:
```
[HH:MM:SS.mmm]  REQ ApiInfoService.octaneVersion
[HH:MM:SS.mmm]  RES ApiInfoService.octaneVersion 0ms
[HH:MM:SS.mmm]  ERR ApiItemService.isGraph 0ms code=5 Not found
```

## SDK Lifecycle

```
1. apiMode_Shared_start("PLORTEST", authCallback, true)
2. apiMode_activate(&error, false)     // license check
3. [run gRPC server, handle RPCs]
4. apiMode_Shared_exit()               // shutdown
```

Auth uses HMAC-SHA256 via Windows CNG (`bcrypt.lib`). Keys in `SERVKEY.cpp` from PLORTEST plugin key files.

## Proto Compatibility

octaneServGrpc serves the **exact same Beta 2 protos** as octane.exe's built-in gRPC server. This means:
- octaneWebR's `OctaneGrpcClientBase` connects without changes
- MCP's gRPC client connects without changes
- No API version translation needed (no Alpha 5 compat layer)
- 96 proto files compiled to C++ server stubs via protoc
