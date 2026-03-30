# Quick Start

**octaneServGrpc** is a C++ gRPC server that embeds the Octane Render SDK. AI agents and octaneWebR connect to it on port 51022 to build and render 3D scenes.

## Prerequisites

- Visual Studio 2022 (C++ desktop workload)
- CMake 3.16+
- Octane Render SDK 2026.2 at `../OctaneRenderSDK_Studio+_2026_2_win/`
- Valid Octane subscription (falls back to demo mode without)

## Build

```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"                    # ~4 min first time (downloads gRPC)
cmake --build . --config Release --target octaneServGrpc  # ~3 min
```

## Run

```bash
build/Release/octaneServGrpc.exe                       # listens on 127.0.0.1:51022
```

Verify: `powershell Get-NetTCPConnection -LocalPort 51022`
Log: `build/Release/log_serv.log` (level: debug by default)

## Test

```bash
# Via MCP tools (from octaneWebR):
get_octane_version       # verify connection + build number
reset_project            # clean scene
place_geo(sphere)        # create geometry
fit_camera               # frame it
start_render             # render
save_render              # save PNG, verify visually
```

## Next Steps

- [docs/BUILD.md](docs/BUILD.md) — Build details, runtime issues, logging
- [docs/TESTING.md](docs/TESTING.md) — Test procedures and validation testing
- [docs/TODO.md](docs/TODO.md) — Unimplemented RPCs and roadmap
