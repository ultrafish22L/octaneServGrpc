# octaneServGrpc Quick Start

C++ gRPC server wrapping Octane SDK 2026.2. No separate octane.exe — the server IS the render engine.

## §1 Build

```bash
cd build && cmake .. -G "Visual Studio 17 2022"   # first time ~4 min (downloads gRPC)
cmake --build . --config Release --target octaneServGrpc
```

Output: `build/Release/octaneServGrpc.exe` (~911MB with runtime DLLs)

## §2 Run

```bash
build/Release/octaneServGrpc.exe [port]   # default 51022
```

- Console mode (Debug build) or Windows tray app (Release)
- Verify: `powershell -Command "Get-NetTCPConnection -LocalPort 51022"`
- Kill: `taskkill //F //IM octaneServGrpc.exe`

## §3 Debug

- Log file: `build/Release/log_serv.log`
- Log level: `--log-level=debug` flag or `SERV_LOG_LEVEL` env var
- Levels: verbose > debug > info > warn > off
- gRPC interceptor auto-logs every RPC call

## §4 Test

- Point octaneWebR at port 51022: `npm run dev` in octaneWebR directory
- MCP tools: `get_octane_version`, `get_device_info`, `load_project`
- Scene test: `load_project` with an ORBX, verify `get_scene_tree` returns nodes
- Render test: `start_render`, `save_render` to PNG, verify image is not blank
- Full glass metal DRESS test: see `octaneWebR/docs/mcp/TEST_PLAN.md`
- Note: Render viewport streaming is not yet implemented (callback stub). Use `save_render` to verify renders.
