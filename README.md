# octaneServGrpc

A standalone C++ gRPC server that embeds the **Octane Render SDK 2026.2** and serves the same proto interface that octaneWebR and MCP already speak. No separate octane.exe needed — the server IS the render engine.

## What It Does

- Links `octane.dll` directly — GPU rendering, scene graph, ORBX loading, everything
- Hosts a gRPC server on port 51022 with all 96 Octane proto services
- Windows tray app with activation menu (production) or console mode (debug)

## Build

```bash
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release --target octaneServGrpc
```

First configure downloads gRPC (~4 min). Subsequent builds use cache.

**Requirements:** Visual Studio 2022, CMake 3.16+, Octane SDK at `../OctaneRenderSDK_Studio+_2026_2_win/`

## Run

```bash
# Console mode (debug)
build/Release/octaneServGrpc.exe 51022 --log-level=debug

# Default port 51022
build/Release/octaneServGrpc.exe
```

Then point octaneWebR at it: `npm run dev` (connects to 127.0.0.1:51022).

## Status

**Working:** SDK init/auth, ApiInfo, ApiProjectManager (load/save ORBX), ApiChangeManager, ApiRenderEngine (start/stop/save, devices, clay mode, priority, bounds), LiveLink (camera get/set), ApiItem (get/set attributes by ID, hasAttr), ApiItemArray, ApiNodeGraph (scene tree traversal), ApiNode (create, connect, disconnect, rename, delete, pin enumeration), StreamCallbackService (render image + statistics streaming).

**Not yet implemented:** Full ItemArray iteration, pick intersection. See IMPROVEMENTS.md for backlog.

## License

Requires valid Octane subscription (PLORTEST plugin key). Falls back to demo mode without activation.
