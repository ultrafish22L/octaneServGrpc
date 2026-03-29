## Rules

**Ask the user before guessing on C++ SDK calls.** The user is a C++ expert and knows the Octane SDK well. When in doubt about SDK behavior (evaluate params, pin semantics, node lifecycle), ask — don't iterate through trial-and-error builds. A 10-second question saves a 30-minute rebuild cycle.

**ALL temp files → `temp/`**

## Build & Test

| Command | What |
| ------- | ---- |
| `cmake --build build --config Release` | Incremental build (10-30s) |
| `cmake -B build -DCMAKE_BUILD_TYPE=Release` | First build (~4min) |

Full details: [QUICKSTART.md](QUICKSTART.md), [docs/BUILD.md](docs/BUILD.md)

## Reference

- System design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Service mapping: [docs/REFERENCE.md](docs/REFERENCE.md)
- Debug issues: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)
- Hardening plan: [docs/PLAN.md](docs/PLAN.md)
- Native MCP design: [docs/UNIFY.md](docs/UNIFY.md) (pending)

### Adding a New Service

1. Proto already in `proto/` — compiled to C++ stubs automatically
2. `#include "newservice.grpc.pb.h"` in `grpc_server.cpp`
3. `class NewServiceImpl final : public octaneapi::NewService::Service { ... }`
4. Override methods with SDK calls (wrap in `GRPC_SAFE_BEGIN`/`GRPC_SAFE_END`)
5. Register: `builder.RegisterService(&newService);`
6. Rebuild
