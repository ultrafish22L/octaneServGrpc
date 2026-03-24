## Last Session (2026-03-24)

### Done

- Full glass metal DRESS test passed (all 8 steps)
- Compat layer test passed (same scene on both backends)
- NodeService, ItemService, RenderEngine save/stats — all working
- Callback streaming wired (render images + statistics)
- SetCamera evaluate fix (`setPinValue` with `evaluate=true`)
- Up vector normalization guard (zero-length protection)
- Proto consolidation (single source in `proto/`)
- Build version tracking (`SERV_BUILD` constant + `GetServVersion` RPC)
- GRPC_SAFE exception handling on all RPCs (93 methods wrapped)

### Pending

1. `docs/PLAN.md` §3 — Handle staling validation (uniqueId == 0 check)
2. `docs/UNIFY.md` — Native MCP server (eliminate octaneWebR dependency)

**ALL temp files -> `temp/`**

## Rules

**Ask the user before guessing on C++ SDK calls.** The user is a C++ expert and knows the Octane SDK well. When in doubt about SDK behavior (evaluate params, pin semantics, node lifecycle), ask — don't iterate through trial-and-error builds. A 10-second question saves a 30-minute rebuild cycle.

## Reference

- Build/run/test: [QUICKSTART.md](QUICKSTART.md)
- Build details: [docs/BUILD.md](docs/BUILD.md)
- System design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Service mapping: [docs/REFERENCE.md](docs/REFERENCE.md)
- Debug issues: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)
- Hardening plan: [docs/PLAN.md](docs/PLAN.md)
- Native MCP design: [docs/UNIFY.md](docs/UNIFY.md)

### Adding a New Service

1. Proto already in `proto/` — compiled to C++ stubs automatically
2. `#include "newservice.grpc.pb.h"` in `grpc_server.cpp`
3. `class NewServiceImpl final : public octaneapi::NewService::Service { ... }`
4. Override methods with SDK calls (wrap in `GRPC_SAFE_BEGIN`/`GRPC_SAFE_END`)
5. Register: `builder.RegisterService(&newService);`
6. Rebuild
