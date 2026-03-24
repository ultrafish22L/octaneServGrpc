## Last Session (2026-03-24)

### Done

- Full glass metal DRESS test passed against octaneServGrpc (all 8 steps)
- Alpha 5 compat test passed (same scene, same results)
- NodeService, ItemService, RenderEngine save/stats — all working
- Callback streaming wired (render images + statistics)

### Pending

1. PLAN.md §2 — GRPC_SAFE macro (exception handling on all RPCs)
2. PLAN.md §3 — Handle staling validation (uniqueId == 0 check)
3. UNIFY.md — Native MCP server (mock gRPC pattern, eliminate octaneWebR dependency)
4. Render viewport streaming optimization (DX11 shared surface path — PLAN.md §4B)

**ALL temp files → `temp/`** — test output, debug dumps, scratch. Never pollute project root.

## Reference

### Run 
- Configure: `cd build && cmake .. -G "Visual Studio 17 2022"`
- Build: `cmake --build . --config Release --target octaneServGrpc`
- Run: `build/Release/octaneServGrpc.exe [port]` (default 51022)
- Kill: `taskkill //F //IM octaneServGrpc.exe`
- `QUICKSTART.md` §1 build, §2 run

### Debug 
- Log: `build/Release/log_serv.log`, `--log-level=debug`
- `QUICKSTART.md` §3 logs

### Test 
- Point octaneWebR at port 51022: `npm run dev`
- `QUICKSTART.md` §4 verification
