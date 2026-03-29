# UNIFY: Native MCP Server in octaneServGrpc

> **Goal**: octaneServGrpc serves MCP directly over stdio so Claude can register it as a standalone MCP server — no Node.js, no octaneWebR dependency. The tray app becomes a self-contained AI-controllable render engine.
>
> **Non-goal**: Replace octaneWebR's MCP. It keeps its own server (78 tools incl. SEGA, art direction, vision). Since it just calls gRPC, this change has no bearing on it.

---

## 1. Architecture

### Current State

```
Claude ──stdio──▶ octaneWebR MCP (Node.js, 78 tools)
                        │
                        │ gRPC (TCP :51022)
                        ▼
                  octaneServGrpc.exe
                        │
                        │ C++ API calls
                        ▼
                  Octane SDK (octane.dll)
```

### Target State

```
Claude ──stdio──▶ octaneServGrpc.exe --mcp
                        │
                        │ direct in-process calls (no TCP)
                        ▼
                  Octane SDK (octane.dll)

octaneWebR ──gRPC :51022──▶ octaneServGrpc.exe   (unchanged, runs simultaneously)
```

### Core Pattern: Mock gRPC

The key insight: all SDK functionality is already exposed through `*ServiceImpl` classes in `grpc_server.cpp`. Rather than reimplementing SDK calls, the MCP layer **reuses these classes directly**:

```
MCP JSON-RPC (stdin)
  → parse tool name + params (nlohmann/json)
  → construct protobuf Request message (JsonStringToMessage)
  → call ServiceImpl::Method(mock_ctx, &request, &response)  ← in-process, no TCP
  → serialize protobuf Response to JSON (MessageToJsonString)
  → format as MCP tool result
  → write JSON-RPC response (stdout)
```

This means:
- **Zero code duplication** — every gRPC service method works for both gRPC and MCP clients
- **No transport overhead** — direct function calls, no TCP serialization/deserialization
- **Single source of truth** — fix a bug in a ServiceImpl, both gRPC and MCP get the fix
- **Handle registry shared** — MCP and gRPC operate on the same scene state

---

## 2. New Components

### File Layout

```
src/mcp/
├── mcp_server.h/.cpp           # stdio JSON-RPC protocol handler
├── mcp_tool_registry.h/.cpp    # tool name → {description, schema, handler}
├── mcp_tool_defs.cpp           # tool definitions (Tier 1: ~30 tools)
├── mcp_resource_defs.cpp       # resource definitions (type system, pin layouts)
├── json_proto_bridge.h/.cpp    # JSON ↔ protobuf conversion utilities
└── mock_context.h              # lightweight grpc::ServerContext stand-in
```

### 2.1 MCP Protocol Handler (`mcp_server.h/.cpp`)

Implements the MCP protocol over stdio (JSON-RPC 2.0):

```cpp
class McpServer {
public:
    McpServer(GrpcServer& grpcServer);  // borrows service impls + handle registry

    /// Run the stdio read loop. Blocks until EOF or shutdown.
    void Run();

    /// Signal shutdown (from tray app exit or SIGTERM).
    void Stop();

private:
    void HandleMessage(const nlohmann::json& msg);
    void HandleInitialize(const nlohmann::json& msg);
    void HandleToolsList(const nlohmann::json& msg);
    void HandleToolsCall(const nlohmann::json& msg);
    void HandleResourcesList(const nlohmann::json& msg);
    void HandleResourcesRead(const nlohmann::json& msg);

    void SendResponse(const nlohmann::json& response);
    void SendError(int64_t id, int code, const std::string& message);

    nlohmann::json ReadMessage();  // reads one JSON-RPC message from stdin

    McpToolRegistry mToolRegistry;
    GrpcServer& mGrpcServer;
    std::atomic<bool> mRunning{false};
};
```

**Message framing**: MCP uses newline-delimited JSON on stdio. Each message is one line of JSON followed by `\n`. Read with `std::getline`, parse with `nlohmann::json::parse`.

**Protocol messages handled**:
| Method | Action |
|--------|--------|
| `initialize` | Return server info, capabilities (tools, resources) |
| `initialized` | No-op notification |
| `tools/list` | Return all registered tool definitions |
| `tools/call` | Dispatch to tool handler, return result |
| `resources/list` | Return registered resources |
| `resources/read` | Return resource content |
| `ping` | Return empty result |

### 2.2 Tool Registry (`mcp_tool_registry.h/.cpp`)

```cpp
struct McpToolDef {
    std::string name;
    std::string description;
    nlohmann::json inputSchema;  // JSON Schema object
    std::function<nlohmann::json(const nlohmann::json& params)> handler;
};

class McpToolRegistry {
public:
    void Register(McpToolDef tool);
    const std::vector<McpToolDef>& GetTools() const;
    McpToolDef* Find(const std::string& name);
};
```

### 2.3 JSON ↔ Protobuf Bridge (`json_proto_bridge.h/.cpp`)

Wraps protobuf's built-in JSON serialization:

```cpp
#include <google/protobuf/util/json_util.h>

namespace McpBridge {

/// Convert JSON object to protobuf message. Returns false on parse error.
bool JsonToProto(const std::string& json, google::protobuf::Message* msg);

/// Convert protobuf message to JSON string.
std::string ProtoToJson(const google::protobuf::Message& msg);

/// Construct an ObjectRef proto from a handle (uint64).
/// Used by most tool handlers to reference scene items.
octaneapi::ObjectRef MakeObjectRef(uint64_t handle);

}
```

Protobuf's `JsonParseOptions` with `ignore_unknown_fields = true` gives forward compatibility — new tool params won't break old server versions.

### 2.4 Mock Context (`mock_context.h`)

The existing `ServiceImpl` methods take `grpc::ServerContext*` but only use it for metadata (which MCP doesn't need). A null or default-constructed context suffices:

```cpp
// Most service methods never touch the context.
// For the few that do (deadline checks), provide a no-op.
inline grpc::ServerContext* MockContext() {
    thread_local grpc::ServerContext ctx;
    return &ctx;
}
```

If `ServerContext` can't be default-constructed outside a gRPC call, create a minimal shim struct that satisfies the pointer type. The GRPC_SAFE_BEGIN/GRPC_SAFE_END macros don't reference it.

---

## 3. Tool Mapping

### Tier 1 — Port First (~30 tools)

These are thin wrappers around single gRPC calls. Each tool handler constructs a protobuf request, calls the ServiceImpl, and returns the response as JSON.

#### Info Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `get_octane_version` | `ApiInfo.octaneVersion` + `ApiInfo.tierIdx` | Combine version + license info |
| `get_device_info` | `ApiRenderEngine.getDeviceCount` + `getDeviceName` + `getMemoryUsage` | Multi-call, merge results |
| `list_node_types` | `ApiInfo.getNodeTypes` | Or serve from static cache (see Resources) |

#### Camera Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `get_camera` | `LiveLink.GetCamera` | Direct passthrough |
| `set_camera` | `LiveLink.SetCamera` | Validate up vector (reject zero-length, default {0,1,0}) |

#### Project Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `load_project` | `ApiProjectManager.loadProject` | Clear handle registry after load |
| `save_project` | `ApiProjectManager.saveProject` / `saveProjectAs` | |
| `reset_project` | `ApiProjectManager.resetProject` | Clear handle registry |

#### Render Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `start_render` | `ApiRenderEngine.continueRendering` | Optionally set RT node first |
| `stop_render` | `ApiRenderEngine.stopRendering` | |
| `get_render_status` | `ApiRenderEngine.getRenderStatistics` | Requires callback wiring (Phase dependency) |
| `save_render` | `ApiRenderEngine.saveImage` | File path + format enum |

#### Render Control Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `set_clay_mode` | `ApiRenderEngine.setClayMode` | |
| `get_clay_mode` | `ApiRenderEngine.clayMode` | |
| `set_render_priority` | `ApiRenderEngine.setRenderPriority` | |
| `get_render_priority` | `ApiRenderEngine.renderPriority` | |
| `set_subsample_mode` | `ApiRenderEngine.setSubSampleMode` | |
| `get_subsample_mode` | `ApiRenderEngine.getSubSampleMode` | |

#### Stats Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `get_geometry_stats` | `ApiRenderEngine.getGeometryStatistics` | |
| `get_texture_stats` | `ApiRenderEngine.getTexturesStatistics` | |
| `get_resource_stats` | `ApiRenderEngine.getResourceStatistics` | |
| `get_scene_bounds` | `ApiRenderEngine.getSceneBounds` | |

#### Scene Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `get_scene_tree` | `ApiProjectManager.rootNodeGraph` → `ApiNodeGraph.getOwnedItems` → recursive | Multi-call traversal using handle registry |
| `get_node_info` | `ApiItem.name` + `ApiItem.outType` + `ApiNode.pinCount` + pin iteration | Multi-call, merge results |

#### Node Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `create_node` | `ApiNode.create` | Validate type_id against crash list |
| `delete_node` | `ApiItem.destroy` | Remove from handle registry |
| `connect_nodes` | `ApiNode.connectTo` / `connectToIx` | |
| `disconnect_pin` | `ApiNode.connectTo` with null | |

#### Attribute Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `get_attribute` | `ApiItem.getValueByAttrID` | Type-dispatch (float, float3, int, string, bool, enum) |
| `set_attribute` | `ApiItem.setValueByAttrID` + `ApiChangeManager.update` | Auto-evaluate after set |

#### Animation Tools
| MCP Tool | Service.Method | Notes |
|----------|---------------|-------|
| `get_animation_range` | `ApiItem.getValueByAttrID` (animation attrs) | |
| `get_animation_data` | `ApiItem.getValueByAttrID` | |
| `is_animated` | `ApiItem.hasAttr` | |
| `clear_animation` | `ApiItem.setValueByAttrID` | |

### Tier 2 — Port Later

| Tool | Blocker |
|------|---------|
| `save_render_passes` | Needs AOV enumeration + multi-file save logic |
| `save_render_passes_exr` | Multi-layer EXR assembly |
| `get_enabled_aovs` | Render pass iteration |
| `import_geo` | Accepts .obj/.glb/.gltf — OBJ direct, GLB/glTF via trimesh |

### Tier 3 — Stay in octaneWebR (Not Ported)

| Tools | Reason |
|-------|--------|
| Art direction (6) | VLM API calls (Anthropic, Gemini), composition math |
| SEGA (4) | NL parsing, semantic vectors, pixel analysis |
| Creative (2) | Pure knowledge base (better as JSON data) |
| Vision critic | External VLM API integration |
| Profiling (4) | MCP-specific instrumentation |
| Webapp (1) | Only relevant when octaneWebR is running |

---

## 4. MCP Resources

Static resources served without SDK calls (loaded once at startup):

| URI | Content | Source |
|-----|---------|--------|
| `octane://node-types` | All 724 node types (name, id, category) | Static JSON blob compiled into exe, or loaded from `data/octane-api-cache.json` |
| `octane://node-types/{category}` | Filtered by prefix (MAT, TEX, GEO, etc.) | Same source, filtered at query time |
| `octane://pin-layout/{typeName}` | Pin list for a node type | Same source |
| `octane://compatibility/{pinType}` | Compatible node types for a pin | Same source |
| `octane://attribute-ids` | Global attribute ID constants | Compiled-in map |

The `octane-api-cache.json` from octaneWebR can be copied into octaneServGrpc's data directory and loaded at startup with nlohmann/json.

---

## 5. Registration

### Claude Code (`.mcp.json` in working directory)

```json
{
  "mcpServers": {
    "octane": {
      "command": "build/Release/octaneServGrpc.exe",
      "args": ["--mcp"],
      "env": {
        "SERV_LOG_LEVEL": "info"
      }
    }
  }
}
```

### Claude Desktop (`claude_desktop_config.json`)

```json
{
  "mcpServers": {
    "octane-render": {
      "command": "build/Release/octaneServGrpc.exe",
      "args": ["--mcp"]
    }
  }
}
```

### Command-Line Modes

```bash
# MCP-only mode (for Claude registration)
octaneServGrpc.exe --mcp

# gRPC-only mode (existing behavior, default)
octaneServGrpc.exe [port]

# Dual mode: gRPC server on port + MCP on stdio
octaneServGrpc.exe --mcp --grpc [port]

# Tray app + MCP (Windows, production)
octaneServGrpc.exe --mcp --tray
```

---

## 6. Thread Model

```
Main thread:
  ├─ Parse args
  ├─ SdkEngine::Init()          # SDK lifecycle (existing)
  ├─ if --grpc: spawn gRPC thread
  │     └─ GrpcServer::RunServer()   (existing, blocks on mServer->Wait())
  ├─ if --mcp: run MCP stdio loop
  │     └─ McpServer::Run()          (new, blocks on stdin reads)
  ├─ if --tray: run Win32 message pump
  │     └─ WndProc dispatch           (existing)
  └─ Shutdown: stop gRPC, stop MCP, SdkEngine::Exit()
```

When `--mcp` and `--tray` are both active, the MCP stdio loop runs in its own thread (like the gRPC server) while the main thread runs the Win32 message pump. This allows the tray app to remain responsive.

When `--mcp` is the only flag, the main thread runs the MCP loop directly (no tray, no gRPC). This is the simplest mode for Claude registration.

---

## 7. Implementation Phases

### Phase 1: MCP Protocol Handler
- Add `nlohmann/json` to CMakeLists.txt via FetchContent
- Implement `McpServer` class: stdio read loop, JSON-RPC parsing, response writing
- Handle `initialize`, `ping`, `tools/list` (empty list), `tools/call` (stub)
- Add `--mcp` flag to `main.cpp` argument parsing
- **Test**: Register with Claude Code, verify `initialize` handshake works

### Phase 2: JSON ↔ Protobuf Bridge
- Implement `json_proto_bridge.h/.cpp` using `google::protobuf::util::JsonUtil`
- Implement `mock_context.h`
- Create one proof-of-concept tool: `get_octane_version`
  - Handler constructs empty request proto
  - Calls `InfoServiceImpl::octaneVersion(mock_ctx, &req, &resp)`
  - Serializes response proto to JSON
  - Returns as MCP tool result
- **Test**: Call `get_octane_version` from Claude, verify response

### Phase 3: Tier 1 Tool Definitions
- Implement `McpToolRegistry` with registration API
- Define all ~30 Tier 1 tools with JSON Schema input definitions
- Implement handlers following the mock-gRPC pattern
- Multi-call tools (get_device_info, get_scene_tree) compose multiple ServiceImpl calls
- Validation logic (set_camera up vector, create_node crash types) lives in handlers
- **Test**: Exercise each tool from Claude Code

### Phase 4: Scene Operations + Handle Registry
- Verify handle registry works correctly when shared between gRPC and MCP callers
- `get_scene_tree`: recursive traversal using `NodeGraphServiceImpl` + `ItemArrayServiceImpl`
- `create_node` / `delete_node`: handle registration/deregistration
- `connect_nodes` / `disconnect_pin`: pin index resolution
- **Test**: Build a scene via MCP tools, verify it renders

### Phase 5: Resources
- Copy `octane-api-cache.json` into `data/` directory
- Load at startup, serve via `resources/list` and `resources/read`
- Implement URI routing (`octane://node-types`, `octane://pin-layout/{type}`, etc.)
- **Test**: Claude can query node types and pin layouts

### Phase 6: Dual-Mode + Polish
- Implement `--mcp --grpc [port]` combined mode
- Implement `--mcp --tray` for Windows tray + MCP
- Logging: MCP requests logged through existing `ServerLog` infrastructure
- Error handling: GRPC_SAFE-style wrapping for MCP handlers
- Graceful shutdown on EOF (Claude disconnects) or SIGTERM
- **Test**: Run gRPC + MCP simultaneously, octaneWebR and Claude both control same scene

---

## 8. Dependencies

### New
| Dependency | Purpose | Integration |
|------------|---------|-------------|
| `nlohmann/json` v3.11+ | MCP protocol JSON parsing, tool schemas, resource content | FetchContent (header-only, ~1MB download) |

### Existing (already available)
| Dependency | Purpose |
|------------|---------|
| `protobuf` (bundled in gRPC) | `google::protobuf::util::JsonUtil` for proto ↔ JSON |
| `grpc` | ServiceImpl classes, ServerContext type |
| Octane SDK | All rendering functionality |

No new runtime DLLs. nlohmann/json is header-only.

---

## 9. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| `grpc::ServerContext` can't be constructed outside a gRPC call | Mock pattern breaks | Use a minimal shim struct or refactor ServiceImpl methods to take an optional context. Worst case: extract SDK call logic into shared helper functions called by both gRPC and MCP handlers. |
| Protobuf JSON serialization doesn't handle all field types cleanly | Garbled tool responses | Test with every proto message type used. Use `JsonPrintOptions` with `always_print_primitive_fields = true` to avoid missing defaults. |
| stdin/stdout conflicts with Win32 tray app | MCP transport broken in tray mode | When `--tray` is active, redirect stdio to pipes. Or: MCP mode and tray mode are mutually exclusive (Claude doesn't need the tray). |
| Handle registry thread safety with concurrent gRPC + MCP | Race conditions | Handle registry already uses mutex. Both gRPC and MCP threads will contend on same lock — acceptable since Octane's message thread is single-threaded anyway. |
| Large scene tree traversal blocks MCP | Tool call timeout | Implement depth limit (existing octaneWebR pattern). Return partial tree with continuation hint. |

---

## 10. What Changes in octaneWebR

**Nothing.** octaneWebR's MCP server continues to connect to octaneServGrpc over gRPC on port 51022. It is unaware of the native MCP server. The two MCP servers can even run simultaneously — Claude Code can have both registered, using octaneServGrpc's MCP for core rendering and octaneWebR's MCP for art direction/SEGA/vision tools.

---

## 11. Success Criteria

1. `octaneServGrpc.exe --mcp` starts and completes MCP handshake with Claude
2. All Tier 1 tools callable from Claude Code and return correct results
3. Can build and render a scene entirely through the native MCP (no octaneWebR)
4. Dual mode: gRPC serves octaneWebR while MCP serves Claude on the same running instance
5. No regressions in existing gRPC functionality
