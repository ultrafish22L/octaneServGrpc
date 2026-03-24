# Quick Start

## §1 Build

```bash
cd build && cmake .. -G "Visual Studio 17 2022"   # first time ~4 min (downloads gRPC)
cmake --build . --config Release --target octaneServGrpc
```

Requires: VS 2022, CMake 3.16+, Octane SDK at `../OctaneRenderSDK_Studio+_2026_2_win/`. See [docs/BUILD.md](docs/BUILD.md) for details.

## §2 Run

```bash
build/Release/octaneServGrpc.exe [port]   # default 51022, --log-level=debug for verbose
```

Verify: `powershell -Command "Get-NetTCPConnection -LocalPort 51022"`
Kill: `taskkill //F //IM octaneServGrpc.exe`
Log: `build/Release/log_serv.log`

## §3 Test

1. Point octaneWebR at port 51022: `npm run dev`
2. MCP smoke: `get_octane_version`, `get_device_info`
3. Scene test: `load_project` with ORBX, verify `get_scene_tree` returns nodes
4. Render test: `start_render`, `save_render` to PNG, verify not blank
5. Viewport: verify render images stream to preview
6. Full test: glass metal DRESS — see `octaneWebR/docs/mcp/TEST_PLAN.md`

See [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) for issues.
