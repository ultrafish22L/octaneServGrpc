## Last Session (2026-03-24)

### Done

- Full glass metal DRESS test passed (all 8 steps)
- Compat layer test passed (same scene on both backends)
- NodeService, ItemService, RenderEngine save/stats — all working
- Callback streaming wired (render images + statistics)

### Pending

1. `docs/PLAN.md` §2 — GRPC_SAFE macro (exception handling on all RPCs)
2. `docs/PLAN.md` §3 — Handle staling validation (uniqueId == 0 check)
3. `docs/UNIFY.md` — Native MCP server (eliminate octaneWebR dependency)

**ALL temp files → `temp/`**

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
4. Override methods with SDK calls
5. Register: `builder.RegisterService(&newService);`
6. Rebuild
