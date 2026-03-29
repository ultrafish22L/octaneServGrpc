# Changelog

## v2.4.3 — 2026-03-29

- Version bump to sync with octaneWebR v2.4.3 (no server-side changes)

## v2.4.1 — 2026-03-27

- Robustness: mPitch zero-guard in `grabRenderResult` — skips images with zero pitch instead of sending empty buffer
- Robustness: buffer size overflow protection — `mPitch * mSize.y * bytesPerPixel` checked against `SIZE_MAX` before allocation

## v2.4.0 — 2026-03-24

- GRPC_SAFE exception handling on all RPCs (93 methods, SEH + C++ exceptions via `/EHa`)
- SetCamera evaluate fix — `setPinValue` called with `evaluate=true` so camera updates take effect immediately
- Up vector normalization guard — zero-length up vectors rejected before reaching SDK
- Proto consolidation — single source in `proto/`, no duplicate copies
- Build version tracking — `SERV_BUILD` constant + `GetServVersion` RPC for verifying running binary matches build

## v1.1.0 — 2026-03-24

- ApiNodeService — create, connect, disconnect, rename, delete, pin enumeration, getPinValue
- ApiItemService — get/set attributes by ID, hasAttr, connectedNode, pin count
- ApiRenderEngineService — saveImage, getStatistics, isCompiling, isRunning
- StreamCallbackService — render image + statistics streaming wired
- HandleRegistry — auto-registers pin children on create/connect
- Glass metal DRESS test verified end-to-end (3 spheres + floor via MCP)
- Compat layer tested (see `octaneWebR/docs/mcp/ALPHA5_COMPAT.md`)

## v1.0.0 — 2026-03-22

- Project scaffolding (CMake + FetchContent gRPC, 96 protos, SDK linking, tray app)
- SdkEngine — init, auth (HMAC-SHA256/bcrypt), activate, exit
- ApiInfoService, ApiProjectManagerService, ApiChangeManagerService
- ApiRenderEngineService (devices, clay, priority, bounds, start/stop)
- LiveLinkService (camera get/set)
- ApiItemService (name, type, destroy), ApiItemArrayService, ApiNodeGraphService
- HandleRegistry, ServerLog, LoggingInterceptor
- Verified with octaneWebR + MCP (version, device info, ORBX load, scene tree)
