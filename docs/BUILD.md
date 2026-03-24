# Build Guide

## Prerequisites

- **Visual Studio 2022** with C++ desktop workload
- **CMake 3.16+**
- **Octane Render SDK 2026.2** in sibling directory `OctaneRenderSDK_Studio+_2026_2_win/`
- **Internet** (first configure downloads gRPC ~200MB, ~4 min)

## Commands

```bash
# First build
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"                              # ~4 min first time
cmake --build . --config Release --target octaneServGrpc          # ~3 min

# Incremental (10-30s)
cmake --build . --config Release --target octaneServGrpc

# Clean exe only (keep gRPC cache)
cmake --build . --config Release --target octaneServGrpc --clean-first

# Full clean (re-downloads gRPC)
rm -rf build && mkdir build && cd build && cmake .. -G "Visual Studio 17 2022"
```

## Build Output

```
build/Release/
├── octaneServGrpc.exe      9.7 MB    the server
├── octane.dll              362 MB    Octane render engine
├── octane.dat              129 MB    kernels/data
├── libcef.dll              215 MB    Chromium (SDK dependency)
└── log_serv.log                      runtime log
```

Total ~911 MB. DLLs auto-copied from SDK by post-build step.

## Proto Compilation

96 `.proto` files compiled by protoc → `build/proto_gen/*.pb.cc` → linked into `serv_protos.lib` (154 MB).

Three excluded (duplicates): `apinodesystem.proto`, `apinodearray.proto`, `apiitemarray.proto` — split versions used instead.

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `OCTANE_SDK_DIR` | `../OctaneRenderSDK_Studio+_2026_2_win` | Octane SDK path |

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for build issues.
