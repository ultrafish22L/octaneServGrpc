# octaneServGrpc

A standalone GPU render engine controllable by AI. C++ gRPC server embedding Octane Render SDK 2026.2 — no separate octane.exe needed.

- Links `octane.dll` directly (GPU rendering, scene graph, ORBX, everything)
- 96 Octane proto services on port 51022
- octaneWebR + MCP connect with zero changes
- AI agents build, light, and render photorealistic 3D scenes through natural language

## Architecture

```
 Claude Code / Browser UI / Any gRPC Client
        │
        │  gRPC (HTTP/2, port 51022)
        ▼
 ┌──────────────────────────────────────────────┐
 │  octaneServGrpc.exe                           │
 │                                               │
 │  ┌──────────────────────────────────────┐    │
 │  │  gRPC Server (grpc::ServerBuilder)   │    │
 │  │                                      │    │
 │  │  InfoServiceImpl                     │    │
 │  │  ProjectManagerServiceImpl           │    │
 │  │  RenderEngineServiceImpl             │    │
 │  │  NodeServiceImpl                     │    │
 │  │  NodeGraphServiceImpl                │    │
 │  │  ... (12 service classes total)      │    │
 │  └──────────────┬───────────────────────┘    │
 │                 │                             │
 │  ┌──────────────▼───────────────────────┐    │
 │  │  Handle Registry                     │    │
 │  │  uint64 handle ↔ ApiItem* pointer    │    │
 │  └──────────────┬───────────────────────┘    │
 │                 │                             │
 │  ┌──────────────▼───────────────────────┐    │
 │  │  Octane SDK (octane.dll)             │    │
 │  │  C++ API: ApiInfo, ApiNode,          │    │
 │  │  ApiRenderEngine, ApiProjectManager  │    │
 │  └──────────────────────────────────────┘    │
 └──────────────────────────────────────────────┘
```

## Thread Model

- **Main thread**: Win32 message pump (tray icon) or console input loop
- **AppThread**: runs `GrpcServer::RunServer()` (blocks on `mServer->Wait()`)
- **gRPC thread pool**: handles concurrent client RPCs
- **SDK**: called from gRPC threads; SDK is internally thread-safe

## Handle Registry

Clients reference SDK objects by `uint64` handles, not raw pointers.

| Handle Range | Type | Lifetime | Source |
|---|---|---|---|
| Low range | `ApiItem*` (nodes, graphs) | SDK-managed | `ApiItem::uniqueId()` |
| High range (2^52+) | `ApiItemArray*` | Registry-owned (60s TTL) | Synthetic counter |

Cleared on `resetProject()` / `loadProject()`. Stale handles auto-evicted when `uniqueId() == 0`.

## Hardening

Every RPC is wrapped in `GRPC_SAFE_BEGIN/END` — catches C++ exceptions and (via `/EHa`) Windows SEH. The server never crashes from bad input.

All inputs validated with descriptive errors: handle lookups, bounds checks, type mismatch detection, pre-render validation. See `docs/BUILD.md` § Server-Side Validation.

## Proto System

The 96 `.proto` files in `proto/` are machine-generated from Octane SDK C++ headers. Each SDK class becomes a proto service, each method becomes an RPC. CMake compiles these via `protoc` + `grpc_cpp_plugin` into C++ stubs. Hand-written service classes inherit those stubs and forward every call to the SDK.

The same protos generate TypeScript bindings for octaneWebR (browser UI + MCP server). Because gRPC has official codegen for 10+ languages, these protos are a universal interface definition — any language with a gRPC stack can drive Octane.

## SDK Lifecycle

```
1. apiMode_Shared_start("PLORTEST", authCallback, true)
2. apiMode_activate(&error, false)     // license check
3. [run gRPC server, handle RPCs]
4. apiMode_Shared_exit()               // shutdown
```

Auth uses HMAC-SHA256 via Windows CNG (`bcrypt.lib`). Requires valid Octane subscription (PLORTEST plugin key). Falls back to demo mode without activation.

## Logging

Three layers:
1. **gRPC Interceptor** — auto-logs every RPC, acts as SDK readiness guard
2. **ServerLog** — level-filtered file + Octane log window (dual output)
3. **SDK log flag** — lifecycle events to Octane's own log system

Default level: `debug`. Log file: `build/Release/log_serv.log`. See `docs/BUILD.md` § Logging.

## Quick Start

See [QUICKSTART.md](QUICKSTART.md). See [docs/](docs/) for build details, testing, and roadmap.
