# Changelog

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
