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

## Build Issues

**"Octane SDK not found"** — SDK path is relative (`../OctaneRenderSDK_Studio+_2026_2_win`). Override: `cmake .. -DOCTANE_SDK_DIR="C:/path/to/sdk"`

**"Permission denied" on .obj** — Stale build processes. `taskkill /F /IM cl.exe && taskkill /F /IM MSBuild.exe`, then rebuild.

**CMakeCache path mismatch** — After moving the project: `rm -rf build && mkdir build && cd build && cmake ..`

**CRT mismatch** — Project uses `MultiThreadedDLL` (dynamic CRT) to match gRPC's BoringSSL. Don't mix static/dynamic.

**FetchContent slow** — First configure downloads ~200MB (3-5 min). Cache is in `build/_deps/` — don't delete it.

## Runtime Issues

**Port 51022 in use** — `powershell Get-NetTCPConnection -LocalPort 51022` to find PID, then `taskkill /F /PID <pid>`. Or run on different port: `octaneServGrpc.exe 51023`.

**"FAILED to initialize Octane SDK"** — Missing `octane.dll` (rebuild triggers copy), wrong SDK version, or tier mismatch (expects tier 2 Studio+).

**Viewport empty** — Check MCP relay on port 51023 (`powershell Get-NetTCPConnection -LocalPort 51023`). If not running, restart MCP process — see `octaneWebR/docs/mcp/BUILD.md` §2 SCRATCH. Also verify RT exists and `start_render` called.

**Scene outliner empty** — Check `log_serv.log` for `ApiItemArrayService.size` errors. "Array not found" = handle registry miss. Synthetic array handles should auto-register; restart the server if stale.

**MCP 120s timeout** — Load is fast (<100ms). Timeout means callback streaming not connected. Check `log_mcp.log` for `Callback streaming started`.

## Logging

Four log files across the stack. Read all of them when debugging — the same failure appears differently at each layer.

| File | Location | Source | Default Level |
|------|----------|--------|---------------|
| `log_serv.log` | `build/Release/` (next to exe) | gRPC server (interceptor + ServerLog) | `debug` |
| `log_grpc.log` | `octaneWebR/` root | gRPC client (OctaneGrpcClientBase + Vite proxy) | `debug` |
| `log_mcp.log` | `octaneWebR/` root | MCP server (tool execution) | `debug` |
| `log_client.log` | `octaneWebR/` root | Browser JS errors (batched via `/api/log`) | `info` |

`log_serv.log` and `log_grpc.log` use the same format and can be diffed side-by-side:
```
[HH:MM:SS.mmm]  REQ ServiceName.methodName {details}
[HH:MM:SS.mmm]  RES ServiceName.methodName {elapsed}ms
[HH:MM:SS.mmm]  ERR ServiceName.methodName {error}
```

### Log Levels

Server log level is set via `--log-level=<level>` or `SERV_LOG_LEVEL` env var:

| Level | File Output | Octane Log Window |
|-------|-------------|-------------------|
| `verbose` | ALL RPC calls (firehose) | ALL RPC calls |
| `debug` | Mutating + lifecycle + curated reads | Mutating + lifecycle + curated reads |
| `info` | Mutating + lifecycle + errors | Mutating + lifecycle + errors |
| `warn` | Errors only | Errors only |
| `off` | Nothing | Mutating + lifecycle + errors (always forwarded) |

The Octane log window sink fires independently of the file log level — errors and mutating operations always appear in the log window, even with `--log-level=off`.

### Log Patterns

| Pattern | Meaning |
|---------|---------|
| `REQ ... RES 0ms` | Success |
| `ERR code=12` | Unimplemented (proto method not wired to SDK) |
| `ERR code=5` | Not found (stale handle or missing node) |
| `ERR code=3` | Invalid argument (bad input from client) |
| `ERR code=9` | Failed precondition (e.g. no RT set) |

### Verbose Mode

```bash
octaneServGrpc.exe --log-level=verbose   # logs every RPC call to file
```

## Server-Side Validation

The server validates all inputs and returns descriptive gRPC errors. AI agents should NOT duplicate this logic — just call the server and act on the error message.

| Validation | Error Code | Example Message |
|-----------|-----------|-----------------|
| Handle not found | NOT_FOUND | `"item handle 9999 not found in registry (stale or never registered)"` |
| Invalid argument | INVALID_ARGUMENT | `"clay mode value 5 is not valid (valid: 0=none, 1=grey, 2=color, 3=color+wireframe)"` |
| Type mismatch | INVALID_ARGUMENT | `"type mismatch on attribute 185: SDK type is 9 but you sent string"` |
| Missing precondition | FAILED_PRECONDITION | `"render target has no geometry connected to pin 3 (mesh)"` |
| SDK not ready | UNAVAILABLE | `"SDK not ready (engine not initialized or not started)"` |
| Port conflict | stderr + log | `"Port 51022 is likely in use by another process"` |
| Connection failed | WRN log | `"connection to pin \"camera\" did not take effect — check pin type compatibility"` |
| Save failed | ERR log | `"saveImage returned false — verify path exists, render is active"` |

## Design Notes

- Handle registry is in-memory (cleared on server restart, loadProject, resetProject). Stale handles are auto-evicted when the SDK reports `uniqueId == 0`.
- Array handles have a 60-second TTL — purged automatically to prevent accumulation.
