# octaneServGrpc — Hardening & Architecture Overhaul

## §1 Context

Deep code review of octaneServGrpc revealed fundamental issues that would make the server crash-prone under real-world use. The server must be **bulletproof** — it wraps a GPU render engine that's expensive to restart, so any crash means lost render state, lost time. The current code has zero exception handling, an unnecessary caching layer, and the callback system (the lifeblood of the viewport) is a stub.

Goals:
1. **Never crash** — wrap every SDK call, validate every input, survive any malformed gRPC request
2. **Minimal state** — use Octane's own graph as the source of truth, eliminate redundant caching
3. **Callbacks done right** — especially render image streaming and DX11 shared surfaces
4. **Production-grade** — fuzzing-friendly, observable, recoverable

---

## §2 Hardening — ✅ COMPLETED (v2.4.0–v2.4.4)

> All items in §2 are implemented. See CHANGELOG.md for details.
> - 1A–1B: GRPC_SAFE macro + requireItem/requireNode/requireGraph/requireArray (v2.4.0, 98/98 methods)
> - 1C: Input bounds checks — device index, array bounds, buffer overflow, pin index (v2.4.0–v2.4.4)
> - 1D: SDK readiness checked at startup
> - 1E: Descriptive error messages on every failure path
> - 1F: SEH exception handling via `/EHa` (v2.4.0)

### 1A. Global exception boundary around every RPC

**Problem**: Zero try/catch anywhere. A single bad SDK call (null pointer, invalid handle, SDK assertion) kills the process.

**Fix**: Wrap every service method with a macro/helper that catches everything:

```cpp
// In grpc_server.cpp — new macro
#define GRPC_SAFE(body) \
    try { body } \
    catch (const std::exception& e) { \
        ServerLog::instance().err(SERVICE_NAME, __func__, e.what()); \
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what()); \
    } catch (...) { \
        ServerLog::instance().err(SERVICE_NAME, __func__, "unknown exception"); \
        return grpc::Status(grpc::StatusCode::INTERNAL, "internal error"); \
    }
```

Every service method becomes:
```cpp
grpc::Status someMethod(...) override {
    GRPC_SAFE(
        // actual implementation
    )
}
```

**Files**: `src/grpc_server.cpp`

### 1B. Handle validation on every lookup

**Problem**: Many methods silently return OK with empty responses when handle lookup fails (e.g., `ItemServiceImpl::isGraph` returns `false` for invalid handles instead of an error). Other methods like `setRenderTargetNode` blindly cast without null check — `reinterpret_cast<Octane::ApiNode*>(item)` where item could be nullptr.

**Fix**: Centralized validated lookup helper:

```cpp
// Returns item or sets gRPC error status. Caller checks and early-returns.
Octane::ApiItem* getItemOrError(uint64_t handle, grpc::Status& status) {
    if (handle == 0) {
        status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "null handle");
        return nullptr;
    }
    auto* item = sHandleRegistry->Lookup(handle);
    if (!item) {
        status = grpc::Status(grpc::StatusCode::NOT_FOUND, "handle not found: " + std::to_string(handle));
        return nullptr;
    }
    return item;
}
```

Apply to every service method that takes an ObjectRef. The `isGraph`/`isNode` returning false for invalid handles masks bugs — return NOT_FOUND instead.

**Files**: `src/grpc_server.cpp`

### 1C. Input validation

**Problem**: No bounds checking anywhere. `getDeviceName(request->index())` passes unchecked index to SDK. `loadProject(path)` passes empty strings. Array `get(index)` has no bounds check.

**Fix**:
- `getDeviceName`: check `index < getDeviceCount()`
- `getMemoryUsage`: check `deviceix` bounds
- `loadProject`/`saveProjectAs`: check non-empty path
- `ApiItemArray::get`: check `index < size()`
- All enum casts: validate range before `static_cast`

**Files**: `src/grpc_server.cpp`

### 1D. SDK readiness guard

**Problem**: If SDK isn't initialized/activated and a gRPC call comes in, raw SDK calls will crash or return garbage.

**Fix**: Check `SdkEngine::IsReady()` at server startup, and add a global interceptor check or per-service guard that returns `UNAVAILABLE` if SDK not ready.

**Files**: `src/grpc_server.cpp`, `src/util/logging_interceptor.h` (add readiness check to interceptor)

### 1E. Descriptive error responses on every failure

**Principle**: Every bad request gets a gRPC error response with a **full human-readable description** of what went wrong. Never return a vague "internal error" when we can say exactly what failed. This applies even under fuzz — the server stays up and tells the caller precisely why their request was rejected.

**Examples of good error messages**:
- `"handle 0 is invalid (must be non-zero uniqueId)"`
- `"handle 9999 not found in registry (stale or never registered)"`
- `"getDeviceName index 5 out of range (device count is 2)"`
- `"loadProject path is empty"`
- `"ApiItemArray::get index 42 out of bounds (array size is 10)"`
- `"setClayMode value 99 is not a valid ClayMode enum (valid: 0-3)"`
- `"item 1234 is not a graph (cannot call getOwnedItems on a node)"`
- `"SDK not ready (engine not initialized or license not activated)"`
- `"SDK exception in ApiNode::create: <exception.what()>"`
- `"SEH exception 0xC0000005 (access violation) in ApiRenderEngine::saveImage"`

**Implementation**: The `GRPC_SAFE` macro, `getItemOrError` helper, and all input validation paths must format a descriptive string into the gRPC Status error_message. Include: the method context, the bad value, what was expected, and the SDK error if available. Log the same string to `log_serv.log` at ERR level.

**Files**: `src/grpc_server.cpp`

### 1F. Structured Error Logging (SEH on Windows)

**Problem**: If the SDK triggers a Windows SEH exception (access violation, etc.), the process dies with no information.

**Fix**: Add `__try/__except` in the top-level RPC macro on Windows to catch SEH and convert to gRPC INTERNAL error + log the exception code. This is the last-resort safety net.

**Files**: `src/grpc_server.cpp`

---

## §3 Simplify State — ✅ 2A COMPLETED, 2B OPEN

### 2A. HandleRegistry — ✅ COMPLETED (v1.1.0)

Implemented as designed: validated thin map with auto-eviction on `uniqueId == 0`. See `src/util/handle_registry.h`.

### 2B. Short-lived arrays — OPEN

**Problem**: `ApiItemArray*` objects are heap-allocated in `getOwnedItems()` but never freed unless the client explicitly calls something that triggers `Clear()`. They accumulate.

**Fix**: Arrays should be ephemeral. Two options:
- **Option A**: Return array contents inline in the response (repeated ObjectRef). No array handle needed.
- **Option B**: Keep array handles but add a TTL / auto-cleanup. After 60s with no access, delete.

**Recommendation**: Option A for `getOwnedItems`. Eliminates array leak entirely.

**Files**: `src/grpc_server.cpp`, `src/util/handle_registry.h`

---

## §4 Callbacks — ✅ COMPLETED (v1.0.0–v1.1.0)

> Callbacks fully implemented: render image streaming (buffer + DX11 shared surface),
> statistics, render failure, project change notifications. See CHANGELOG.md v1.1.0.

### 3A. Wire StreamCallbackService to SDK callbacks

**Problem**: Current stub just sleeps in a loop. No render images, no statistics, no project change notifications reach the client.

**Architecture**:

```
SDK callback thread                    gRPC stream thread
        |                                      |
   onNewImage()                                 |
        |                                      |
        +-- push to lock-free queue ---------->|
        |                                  pop + write to stream
   onNewStatistics()                            |
        |                                      |
        +-- push to lock-free queue ---------->|
```

**Implementation**:

```cpp
class StreamCallbackServiceImpl {
    // Thread-safe queue for callback events
    struct CallbackEvent {
        enum Type { NewImage, NewStatistics, RenderFailure, ProjectChanged };
        Type type;
        uint64_t userData;
        // For NewImage: render image data (or shared surface handle)
    };

    std::mutex mQueueMutex;
    std::condition_variable mQueueCV;
    std::queue<CallbackEvent> mQueue;
    std::atomic<bool> mHasClient{false};

    // SDK callbacks — registered once at server startup
    static void onNewImageCallback(void* userData) {
        auto* self = static_cast<StreamCallbackServiceImpl*>(userData);
        self->enqueue(CallbackEvent{CallbackEvent::NewImage, 0});
    }

    grpc::Status callbackChannel(..., grpc::ServerWriter<StreamCallbackRequest>* writer) override {
        mHasClient = true;
        while (!context->IsCancelled()) {
            CallbackEvent event;
            {
                std::unique_lock<std::mutex> lock(mQueueMutex);
                mQueueCV.wait_for(lock, 200ms, [&]{ return !mQueue.empty(); });
                if (mQueue.empty()) continue;
                event = std::move(mQueue.front());
                mQueue.pop();
            }
            // Build proto message from event, write to stream
            StreamCallbackRequest msg;
            // ... populate based on event.type
            if (!writer->Write(msg)) break; // client disconnected
        }
        mHasClient = false;
        return grpc::Status::OK;
    }
};
```

**Key design decisions**:
- Register SDK callbacks in `GrpcServer::RunServer()`, before the server starts accepting
- Use condition_variable (not busy-spin) for the stream loop
- Support multiple concurrent callback streams (multiple clients)
- Queue has a max depth (e.g., 100) — drop oldest if overflowing (render images are temporal)

**Files**: `src/grpc_server.cpp` (StreamCallbackServiceImpl rewrite), `src/sdk_engine.cpp` (callback registration)

### 3B. Render Image Streaming — Two Paths

#### Path 1: Buffer copy (gRPC bytes, works everywhere)
- `grabRenderResult()` → gets `ApiArrayApiRenderImage` with pixel buffer
- Serialize buffer into `StreamCallbackRequest.newImage` proto message
- Client receives bytes, decodes to texture
- **Pro**: Works over network, no GPU sharing needed
- **Con**: Bandwidth-heavy for high-res, latency from copy

#### Path 2: DX11 Shared Surface (zero-copy, local only)
- `setSharedSurfaceOutputType(SHARED_SURFACE_TYPE_D3D11)` → Octane renders directly to a DX11 shared texture
- `ApiRenderImage.sharedSurface` → handle to the DX11 resource
- Client opens the same DX11 shared surface by handle and composites directly
- **Pro**: Zero-copy, minimal latency, GPU-to-GPU
- **Con**: Local only (same machine), requires DX11 on both sides

**Plan**: Implement both. Buffer copy is the default (always works). DX11 shared surface is opt-in when client requests it via `setSharedSurfaceOutputType`. The callback stream sends either the buffer or just the shared surface handle depending on mode.

**Files**: `src/grpc_server.cpp` (RenderEngineServiceImpl — add shared surface methods + grabRenderResult/releaseRenderResult)

### 3C. Statistics & Failure Callbacks

Wire `setOnNewStatisticsCallback` and `setOnRenderFailureCallback` to push events to the same queue. These are lightweight (no image data) and critical for UI feedback.

**Files**: `src/grpc_server.cpp`

### 3D. ChangeManager Observer

The `callback.proto` defines `ChangeManagerObserver` with `ChangeEvent` (ITEM_ADDED, ITEM_DELETE, CONNECTION_CHANGED, etc.). Wire this to push through the stream. This is how octaneWebR knows to refresh its scene tree without polling.

**Files**: `src/grpc_server.cpp`

---

## §5 API Surface — ✅ COMPLETED (v1.1.0)

> ApiNodeService and ApiItemService fully implemented. See CHANGELOG.md v1.1.0.

### 4A. ApiNodeService — Scene Building

**Methods needed** (from `apinodesystem_7.proto`):
- `create(NodeType, name)` → create node in graph, return uniqueId
- `connectTo(source, target, pinIndex)` → wire nodes
- `connectToIx(source, target, pinIndex)` → wire by index
- `disconnectPin(node, pinIndex)` → unwire
- `setPinValue(node, pinId, value)` / `getPinValue(node, pinId)` → read/write pin values
- `pinCount(node)` / `staticPinCount(node)` → introspection

All methods must: validate inputs, catch SDK exceptions, return proper gRPC errors.

**Files**: `src/grpc_server.cpp` (NodeServiceImpl)

### 4B. ApiItemService — Attribute Access

**Methods needed** (from `apinodesystem_3.proto`):
- `getByAttrID(item, attrId)` → get attribute value
- `setByAttrID(item, attrId, value)` → set attribute value
- `hasAttr(item, attrId)` → check if attribute exists
- `connectedNode(item, pinId)` → get connected node

**Files**: `src/grpc_server.cpp` (ItemServiceImpl)

---

## §6 Future Ideas

### 5A. Health & Diagnostics Endpoint
- New gRPC service or extend ApiInfo: `getServerHealth()` → returns SDK ready, GPU count, memory, active streams, uptime, queue depth
- Lets octaneWebR show real status in the connection LED

### 5B. Graceful Degradation
- If GPU runs out of memory, return `RESOURCE_EXHAUSTED` not crash
- If SDK enters error state, return `UNAVAILABLE` and attempt recovery
- Watchdog thread: if SDK stops responding (callback queue stalls), log + alert

### 5C. Rate Limiting / Back-Pressure
- Render image callbacks can be extremely high frequency
- If the gRPC stream write falls behind, drop frames (keep latest only)
- Track callback latency (time from SDK callback to stream write)
- Configurable frame rate cap for the callback stream (e.g., 30fps max)

### 5D. Multi-Client Support
- Multiple octaneWebR instances or MCP + octaneWebR simultaneously
- Each client gets its own callback stream
- Shared read operations are safe (SDK is thread-safe)
- Mutating operations need sequencing (queue + single writer pattern, or trust SDK's internal locking)

### 5E. Render Pass Routing
- `grabRenderResult` returns images for ALL active render passes
- Stream should tag each image with its render pass ID
- Client can subscribe to specific passes (beauty, denoised, AOVs)
- This enables octaneWebR to show multi-pass compositor

### 5F. Pick Intersection (Mouse → Scene)
- Proto already defines `ApiRenderEngine_PickIntersection`
- Implement `pickIntersection(x, y)` → returns hit node, material, position, normal
- Critical for octaneWebR's scene tree selection from viewport click

### 5G. Fuzzing Readiness
- With the GRPC_SAFE macro, the server survives any input
- Add a `--fuzz-mode` flag that enables extra verbose logging of all rejected inputs
- Consider adding a gRPC reflection service for automated fuzzer tooling
- Document the fuzzing test plan: random handles, oversized strings, out-of-range enums, concurrent streams

### 5H. Metrics / Telemetry
- Track per-method call count, latency P50/P99, error rate
- Track callback stream throughput (events/sec, bytes/sec)
- Expose via `getServerMetrics()` RPC or log periodically
- This data is gold for diagnosing performance issues between octaneWebR and the server

### 5I. Connection Lifecycle
- Detect client disconnect cleanly (cancel running operations if needed)
- `ServerReflection` service for debugging (standard gRPC)
- Configurable keepalive settings to detect dead connections faster

---

## §7 Implementation Order

| Phase | What | Status |
|-------|------|--------|
| **1** | Hardening: GRPC_SAFE macro, handle validation, input validation, SDK guard | ✅ v2.4.0–v2.4.4 |
| **2** | Simplify: Validate cached pointers, short-lived arrays | ✅ 2A done, 2B open |
| **3** | Callbacks: Wire StreamCallbackService, render image buffer path | ✅ v1.1.0 |
| **4** | DX11 Shared Surface: setSharedSurfaceOutputType, zero-copy path | ✅ v1.1.0 |
| **5** | ApiNodeService: create, connect, setPinValue | ✅ v1.1.0 |
| **6** | ApiItemService: getByAttrID, setByAttrID, connectedNode | ✅ v1.1.0 |
| **7** | Statistics/Failure/ChangeManager callbacks | ✅ v1.1.0 |
| **8** | Health endpoint, metrics, pick intersection | Open |
| **9** | Multi-client, rate limiting, render pass routing | Open |
| **10** | Fuzzing test suite | Open |

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/grpc_server.cpp` | GRPC_SAFE macro, all service implementations, callback wiring |
| `src/grpc_server.h` | StreamCallbackService state (queue, callbacks), health data |
| `src/util/handle_registry.h` | Add staleness validation, array TTL |
| `src/util/handle_registry.cpp` | Validated lookups, auto-eviction |
| `src/sdk_engine.h` | Add callback registration API |
| `src/sdk_engine.cpp` | Register SDK callbacks, shared surface setup |
| `src/util/logging_interceptor.h` | Add SDK readiness check |
| `src/main.cpp` | Add --fuzz-mode flag, graceful shutdown improvements |

## Verification

1. **Build**: `cmake --build . --config Release` — must compile clean
2. **Smoke test**: Start server, connect octaneWebR, verify version handshake
3. **Load project**: Load ORBX/teapot.orbx, verify scene tree traversal works with validated lookups
4. **Callback test**: Verify render image frames arrive in octaneWebR viewport
5. **Crash test**: Send invalid handles (0, MAX_UINT64, random), empty strings, out-of-range enums — server must return errors, never crash
6. **Multi-client**: Connect octaneWebR + MCP simultaneously, verify both get callbacks
7. **Memory**: Run for 30min under load, verify no handle/array leaks
