# Build Guide

## Prerequisites

- **Visual Studio 2022** (Professional or Community) with C++ desktop workload
- **CMake 3.16+** (3.29 tested)
- **Octane Render SDK 2026.2** at `C:\otoyla\GRPC\dev\OctaneRenderSDK_Studio+_2026_2_win\`
- **Internet** (first configure downloads gRPC ~200MB)

## First Build

```bash
cd C:\otoyla\GRPC\dev\octaneServGrpc
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"        # ~4 min (downloads gRPC)
cmake --build . --config Release --target octaneServGrpc   # ~3 min
```

## Subsequent Builds

```bash
cd build
cmake --build . --config Release --target octaneServGrpc   # ~10-30s (incremental)
```

Only recompiles changed source files. Proto library (`serv_protos.lib`) is cached.

## Clean Rebuild

```bash
# Just the exe (keep gRPC/proto cache):
cmake --build . --config Release --target octaneServGrpc --clean-first

# Full clean (re-downloads gRPC — avoid unless necessary):
cd ..
rm -rf build
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

## Build Output

```
build/Release/
├── octaneServGrpc.exe      # 9.7 MB — the server
├── octane.dll              # 362 MB — Octane render engine
├── octane.dat              # 129 MB — Octane kernels/data
├── libcef.dll              # 215 MB — Chromium (needed by SDK)
├── libGLESv2.dll           # 7.9 MB
├── libEGL.dll              # 483 KB
├── vk_swiftshader.dll      # 5.1 MB — Vulkan fallback
├── d3dcompiler_47.dll      # 4.7 MB
├── *.pak                   # CEF resources
└── log_serv.log            # Runtime log (created on first run)
```

Total: ~911 MB. All DLLs are auto-copied from the SDK by post-build step.

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `OCTANE_SDK_DIR` | `../OctaneRenderSDK_Studio+_2026_2_win` | Path to Octane SDK |
| `CMAKE_BUILD_TYPE` | Release | Build configuration |

## Proto Compilation

96 `.proto` files in `proto/` are compiled by `protoc` + gRPC plugin during build:
- Input: `proto/*.proto`
- Output: `build/proto_gen/*.pb.cc`, `*.pb.h`, `*.grpc.pb.cc`, `*.grpc.pb.h`
- Linked into `serv_protos.lib` (154 MB static library)

Three proto files are excluded (duplicates of split versions):
- `apinodesystem.proto` → use `apinodesystem_1` through `_8` instead
- `apinodearray.proto` → use `apinodesystem_5.proto`
- `apiitemarray.proto` → use `apinodesystem_1.proto`

## Adding a New Service

1. The proto is already in `proto/` and compiled to C++ stubs
2. Add `#include "newservice.grpc.pb.h"` in `grpc_server.cpp`
3. Create `class NewServiceImpl final : public octaneapi::NewService::Service { ... }`
4. Override methods with SDK calls
5. Register in `RunServer()`: `builder.RegisterService(&newService);`
6. Rebuild

## Troubleshooting Build Issues

See `TROUBLESHOOTING.md` — covers permission denied, CMake cache, CRT mismatch, FetchContent slow.
