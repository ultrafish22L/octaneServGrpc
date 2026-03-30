# Troubleshooting

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

## Debugging

### Log Files

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

## Known Limitations

- In-memory registry only (lost on restart). Handle staling validated since v1.1.0 (auto-evicts on `uniqueId == 0`).
