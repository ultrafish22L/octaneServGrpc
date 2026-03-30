# TODO

## Unimplemented RPCs

| Proto Service | RPC | Notes |
|---|---|---|
| `ApiMaterialXGlobal` | `importMaterialXFile` | MCP tool `import_materialx` calls this. Returns UNIMPLEMENTED. |
| `ApiRenderEngine` | `pickIntersection` | Viewport click → scene node. Proto exists, not wired. |
| `ApiNodeService` | `setPinValueByIx`, `setPinValueByPinID`, `setPinValueByName` | SDK limitation — these RPCs are stubs in the SDK itself. |

## Electron Packaging

| Issue | Fix |
|---|---|
| `log_grpc.log` ENOENT in asar | OctaneGrpcClientBase writes to `__dirname` relative path — inside read-only asar in packaged Electron. Use `app.getPath('userData')` instead. |

## Needs Testing

| Issue | Test Case | Notes |
|---|---|---|
| `get_scene_tree` race on large ORBX | Load `starscream.orbx`, immediately call `get_scene_tree` | `load_project` waits for `projectManagerChanged` callback — may already be fixed. |

## SDK Limitations

Known Octane SDK behaviors that cannot be fixed server-side.

| Limitation | Details |
|---|---|
| Render engine calls ignored | `pauseRendering`, `stopRendering`, etc. return success but do nothing |
| Camera not reset after File→Open | LiveLink camera overrides file's saved state |
| `newStatistics` never fires | Statistics callback is a stub in the SDK |
| LiveDB singleton services | `getCategories`, `getMaterials`, `getMaterialPreview`, `downloadMaterial` fail with "invalid pointer type" — gRPC compat layer doesn't handle singleton services (no objectPtr). Tools disabled in MCP `index.ts`. |
| Quad primitive (type 18) | May render nothing on older Octane versions. Works on 2026.2. |

## Roadmap

### Health & Diagnostics

`GetServVersion` returns basic health (sdk_ready, sdk_version, sdk_activated, log_level, handle counts). Still needed:

- Full `getServerHealth()` RPC — GPU count, VRAM usage, active callback streams, uptime, queue depth
- Watchdog thread — detect SDK hangs (callback queue stalls), log + alert
- Return `RESOURCE_EXHAUSTED` on GPU OOM instead of crashing

### Multi-Client

- Multiple octaneWebR + MCP simultaneously
- Each client gets its own callback stream (already works)
- Mutating operations need sequencing (queue + single writer, or trust SDK's internal locking)

### Rate Limiting / Back-Pressure

- Cap callback stream frame rate (e.g., 30fps max)
- If gRPC stream write falls behind, drop frames (keep latest only)
- Track callback latency (time from SDK callback to stream write)

### Render Pass Routing

- `grabRenderResult` returns images for ALL active render passes
- Tag each image with its render pass ID
- Client subscribes to specific passes (beauty, denoised, AOVs)

### Fuzzing

- Server already survives any input (GRPC_SAFE on all RPCs)
- Add `--fuzz-mode` flag for extra verbose logging of rejected inputs
- Add gRPC reflection service for automated fuzzer tooling
- Test plan: random handles, oversized strings, out-of-range enums, concurrent streams

### Metrics / Telemetry

- Per-method call count, latency P50/P99, error rate
- Callback stream throughput (events/sec, bytes/sec)
- Expose via `getServerMetrics()` RPC or log periodically

### Connection Lifecycle

- Clean disconnect detection (cancel running operations if needed)
- `ServerReflection` service for debugging
- Configurable keepalive settings to detect dead connections faster
