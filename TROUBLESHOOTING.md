# Troubleshooting

## Build Issues

### "Octane SDK not found"
The SDK path is relative: `${CMAKE_CURRENT_SOURCE_DIR}/../OctaneRenderSDK_Studio+_2026_2_win`. Ensure the SDK is in the sibling directory `OctaneRenderSDK_Studio+_2026_2_win/`. Or override:
```bash
cmake .. -DOCTANE_SDK_DIR="C:/path/to/sdk"
```

### "Permission denied" on .obj files during build
Stale `cl.exe` / `MSBuild.exe` processes from a previous build. Kill them:
```bash
taskkill /F /IM cl.exe
taskkill /F /IM MSBuild.exe
```
Then rebuild.

### CMakeCache.txt path mismatch
After moving the project directory, delete `build/` entirely and reconfigure:
```bash
rm -rf build && mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
```

### MSVC_RUNTIME_LIBRARY errors
`CMP0091` policy must be set before `project()`. The CMakeLists.txt handles this. If you see runtime mismatch errors, ensure you're not mixing static/dynamic CRT. The project uses `MultiThreadedDLL` (dynamic CRT) to match gRPC's BoringSSL.

### FetchContent takes forever
First configure downloads gRPC + all dependencies (~200MB). Takes 3-5 min. Subsequent configures use the cache in `build/_deps/`. Don't delete `_deps/` unless you want to re-download.

## Runtime Issues

### Port 51022 already in use
Another octaneServGrpc or octane.exe is listening:
```bash
# Check what's on the port
powershell -Command "Get-NetTCPConnection -LocalPort 51022"
# Kill it
taskkill /F /IM octaneServGrpc.exe
taskkill /F /IM octane.exe
```
Or run on a different port: `octaneServGrpc.exe 51023`

### "FAILED to initialize Octane SDK"
- Missing `octane.dll` in the exe directory — rebuild triggers post-build copy
- Wrong SDK version — check that SDK headers match the DLL version
- Tier mismatch — server expects tier 2 (Studio+); check plugin key files

### License not activated
Run with `a` command (console mode) or right-click tray icon → Activation. Requires internet connection to OTOY license server. Falls back to demo mode if activation fails.

### octaneWebR shows "Connected" but viewport is empty
Check that the MCP relay is running on port 51023 (`powershell Get-NetTCPConnection -LocalPort 51023`). If not, the MCP process needs a full restart — see `octaneWebR/docs/mcp/BUILD.md` §2 SCRATCH step 3. Also verify a render target exists and `start_render` has been called.

### Scene outliner empty after ORBX load
Check `log_serv.log` for `ApiItemArrayService.size` errors. If "Array not found", the handle registry isn't finding the `ApiItemArray` returned by `getOwnedItems`. This was fixed by using synthetic array handles (0x8000...) separate from item handles.

### MCP `load_project` reports 120s timeout
The MCP client has a 120s timeout. The actual load is fast (check `log_serv.log` — usually <100ms for ORBX/teapot.orbx). If the timeout occurs, check that callback streaming is connected (`log_mcp.log` should show `Callback streaming started`).

## Debugging

### Enable verbose logging
```bash
octaneServGrpc.exe --log-level=verbose
```
Logs every single RPC call including pin/attribute enumeration. Useful for tracing what octaneWebR sends.

### Check both logs
- **Server**: `build/Release/log_serv.log` — what RPCs arrive and what the server does
- **Client**: `octaneWebR/log_grpc.log` — what RPCs the client sends and receives

### Common log patterns

**Successful call:**
```
[20:18:00.168]  REQ ApiInfoService.octaneVersion
[20:18:00.168]  RES ApiInfoService.octaneVersion 0ms
```

**Unimplemented method:**
```
[20:18:19.537]  REQ ApiItemService.isGraph
[20:18:19.537]  ERR ApiItemService.isGraph 0ms code=12 UNIMPLEMENTED
```
Code 12 = gRPC UNIMPLEMENTED. The service exists but the specific method isn't overridden.

**Handle not found:**
```
[20:23:09.301]  ERR ApiItemArrayService.size 0ms code=5 Array not found
```
Code 5 = NOT_FOUND. The handle from a previous call is stale or the wrong type.

## Known Limitations

- No exception handling on RPC methods (crash-prone on bad input). See PLAN.md §2.
- Handle staling not validated (SDK can delete items, registry keeps stale pointers)
- Handle registry doesn't survive process restart (in-memory only)
- No connection pooling or request batching
- Single-process only (no distributed rendering)
