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

**Scene outliner empty** — Check `log_serv.log` for `ApiItemArrayService.size` errors. "Array not found" = handle registry miss (fixed in v1.0.0 with synthetic array handles).

**MCP 120s timeout** — Load is fast (<100ms). Timeout means callback streaming not connected. Check `log_mcp.log` for `Callback streaming started`.

## Debugging

```bash
octaneServGrpc.exe --log-level=verbose   # logs every RPC call
```

Two logs: `build/Release/log_serv.log` (server RPCs) and `octaneWebR/log_grpc.log` (client RPCs).

Log patterns — success: `REQ...RES 0ms`, unimplemented: `ERR code=12`, not found: `ERR code=5`.

## Known Limitations

- No exception handling on RPCs (crash on bad input) — see `docs/PLAN.md` §2
- Handle staling not validated (stale pointers possible)
- In-memory registry only (lost on restart)
