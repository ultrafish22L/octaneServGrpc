# octaneServGrpc — How It Works

## Executive Summary

octaneServGrpc is a standalone Windows executable that embeds the Octane Render SDK (`octane.dll`) and exposes its entire C++ API over gRPC on port 51022. The 96 `.proto` files in `proto/` are **machine-generated from Octane SDK C++ headers** — each SDK class becomes a proto service, each method becomes an RPC. CMake compiles these protos into C++ stubs via `protoc` + `grpc_cpp_plugin`. Hand-written service classes inherit those stubs and forward every call directly to the SDK. The same proto files also generate TypeScript bindings for the browser UI and MCP server. Because gRPC has official codegen for 10+ languages, these protos are a universal interface definition — any language with a gRPC stack can drive Octane.

---

## 1. Architecture

```
 Claude Code / Browser UI / Any gRPC Client
        │
        │  gRPC (HTTP/2, port 51022)
        ▼
 ┌──────────────────────────────────────────────┐
 │  octaneServGrpc.exe                           │
 │                                               │
 │  ┌──────────────────────────────────────┐    │
 │  │  gRPC Server (grpc::ServerBuilder)   │    │
 │  │                                      │    │
 │  │  InfoServiceImpl                     │    │
 │  │  ProjectManagerServiceImpl           │    │
 │  │  RenderEngineServiceImpl             │    │
 │  │  NodeServiceImpl                     │    │
 │  │  NodeGraphServiceImpl                │    │
 │  │  ... (11 service classes total)      │    │
 │  └──────────────┬───────────────────────┘    │
 │                 │                             │
 │  ┌──────────────▼───────────────────────┐    │
 │  │  Handle Registry                     │    │
 │  │  uint64 handle ↔ ApiItem* pointer    │    │
 │  └──────────────┬───────────────────────┘    │
 │                 │                             │
 │  ┌──────────────▼───────────────────────┐    │
 │  │  Octane SDK (octane.dll)             │    │
 │  │  C++ API: ApiInfo, ApiNode,          │    │
 │  │  ApiRenderEngine, ApiProjectManager  │    │
 │  └──────────────────────────────────────┘    │
 └──────────────────────────────────────────────┘
```

---

## 2. Proto Definitions — Structure and Origin

### Source

The 96 `.proto` files in `proto/` are **machine-generated from the Octane SDK C++ headers**. Every file carries the header:

```
// WARNING: This code is machine generated. Manual changes will be overridden.
```

An external tool (part of the Octane build, not in this repo) parses SDK headers like `apiinfo.h` and emits `apiinfo.proto`.

### Naming Convention

| SDK Header      | SDK Class            | Proto File             | Proto Service          |
|-----------------|----------------------|------------------------|------------------------|
| `apiinfo.h`     | `Octane::ApiInfo`    | `apiinfo.proto`        | `ApiInfoService`       |
| `apirender.h`   | `Octane::ApiRenderEngine` | `apirender.proto` | `ApiRenderService`     |
| `apinodesystem.h` | `Octane::ApiNode` | `apinodesystem_3.proto`| `ApiNodeService`       |

### Message Structure

Each SDK method becomes a Request/Response message pair inside a wrapper message, plus an RPC in the service:

```protobuf
// apiinfo.proto — generated from apiinfo.h

syntax = "proto3";
package octaneapi;

message ApiInfo {
    message octaneVersionRequest {
        // [in] params (none for this method)
    }
    message octaneVersionResponse {
        int32 result = 1;  // [out] return value
    }

    message nodeInfoRequest {
        int32 type = 1;    // [in] NodeType enum
    }
    message nodeInfoResponse {
        ApiNodeInfoProto result = 1;  // [out] struct
    }
    // ... one pair per SDK method
}

service ApiInfoService {
    rpc octaneVersion (ApiInfo.octaneVersionRequest)
        returns (ApiInfo.octaneVersionResponse);
    rpc nodeInfo (ApiInfo.nodeInfoRequest)
        returns (ApiInfo.nodeInfoResponse);
    // ...
}
```

### Shared Types

Five support protos define types used across all services:

| File                    | Contents                                          |
|-------------------------|---------------------------------------------------|
| `common.proto`          | `ObjectRef` (handle wrapper), `ApiItemPtr`         |
| `octaneids.proto`       | Enums: `NodeType`, `AttributeId`, `PinId`          |
| `octaneenums.proto`     | All Octane SDK enums (100+)                        |
| `octaneinfos.proto`     | Struct protos: `ApiNodeInfoProto`, `ApiAttributeInfoProto` |
| `octanerenderpasses.proto` | Render pass ID definitions                      |

### Duplicate Filtering

The SDK generator emits both a monolith `apinodesystem.proto` (11K lines) and split files (`apinodesystem_1.proto` through `_8.proto`). The split files are authoritative; the monolith and two other duplicates are excluded at build time.

---

## 3. Proto Compilation Pipeline

### Overview

```
proto/*.proto
     │
     │  CMake add_custom_command
     │  protoc + grpc_cpp_plugin
     ▼
build/proto_gen/
  ├── apiinfo.pb.h          ← message classes (serialize/deserialize)
  ├── apiinfo.pb.cc
  ├── apiinfo.grpc.pb.h     ← service base class + client stub
  ├── apiinfo.grpc.pb.cc
  ├── apirender.pb.h
  ├── apirender.pb.cc
  ├── ...                   ← ~4 files × 93 protos = ~372 generated files
     │
     │  compiled into
     ▼
serv_protos (static library)
     │
     │  linked into
     ▼
octaneServGrpc.exe
```

### Exact CMake Steps

From `CMakeLists.txt` lines 104–162:

**Step 1 — Glob and filter:**
```cmake
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

# Exclude duplicates (split files are authoritative)
list(FILTER PROTO_FILES EXCLUDE REGEX "apinodesystem\\.proto$")
list(FILTER PROTO_FILES EXCLUDE REGEX "apinodearray\\.proto$")
list(FILTER PROTO_FILES EXCLUDE REGEX "apiitemarray\\.proto$")
```

**Step 2 — Run protoc per file:**
```cmake
foreach(PROTO_FILE ${PROTO_FILES})
    add_custom_command(
        OUTPUT ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
        COMMAND ${_PROTOBUF_PROTOC}
            --proto_path=${PROTO_DIR}
            --proto_path=${PROTOBUF_IMPORT_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
            ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} protobuf::protoc grpc_cpp_plugin
    )
endforeach()
```

Each proto produces exactly **4 files**:

| File                  | Contains                                          |
|-----------------------|---------------------------------------------------|
| `*.pb.h` / `*.pb.cc` | Message classes with getters, setters, serialization |
| `*.grpc.pb.h` / `*.grpc.pb.cc` | `Service` base class (virtual methods to override) + `Stub` class (client-side) |

**Step 3 — Static library:**
```cmake
add_library(serv_protos STATIC ${PROTO_SRCS} ${GRPC_SRCS})
target_link_libraries(serv_protos PUBLIC grpc++ libprotobuf)
```

### Tool Provenance

Both `protoc` and `grpc_cpp_plugin` come from gRPC v1.62.1, auto-downloaded via CMake `FetchContent` on first configure (~200MB, cached in `build/_deps/`).

---

## 4. C++ Service Implementation

### The Pattern

For each proto service, a hand-written C++ class inherits the generated `::Service` base and overrides every RPC method:

```
Generated (apiinfo.grpc.pb.h):          Hand-written (grpc_server.cpp):

class ApiInfoService::Service {          class InfoServiceImpl final
  virtual grpc::Status                     : public octaneapi::ApiInfoService::Service {
    octaneVersion(                           grpc::Status octaneVersion(...) override {
      ServerContext*,                            GRPC_SAFE_BEGIN(SVC)
      const octaneVersionRequest*,                   response->set_result(
      octaneVersionResponse*)                            Octane::ApiInfo::octaneVersion());
    { return UNIMPLEMENTED; }                        return grpc::Status::OK;
};                                               GRPC_SAFE_END(SVC)
                                             }
                                           };
```

### GRPC_SAFE Hardening

Every RPC body is wrapped in `GRPC_SAFE_BEGIN` / `GRPC_SAFE_END` macros:

```cpp
#define GRPC_SAFE_BEGIN(svc)  try {
#define GRPC_SAFE_END(svc)    } catch (const std::exception& e) { \
    return grpc::Status(INTERNAL, svc + "." + __func__ + ": " + e.what()); \
} catch (...) { \
    return grpc::Status(INTERNAL, svc + "." + __func__ + ": unknown exception"); \
}
```

Combined with MSVC `/EHa` flag, this catches both C++ exceptions and Windows SEH (access violations). The server **never crashes** from bad input.

### Handle Registry

Clients reference SDK objects by `uint64` handles, not raw pointers:

| Handle Range      | Type              | Lifetime     |
|-------------------|-------------------|--------------|
| Low range         | `ApiItem*` nodes  | SDK-managed  |
| High range (2^52+)| `ApiItemArray*`   | Registry-owned |

Every handle lookup goes through validated helpers (`requireNode`, `requireGraph`, etc.) that return descriptive `NOT_FOUND` errors for stale handles.

### Service Registration

All services are instantiated and registered in `GrpcServer::RunServer()`:

```cpp
InfoServiceImpl               infoService;
ProjectManagerServiceImpl     projectManagerService;
// ... 11 total

grpc::ServerBuilder builder;
builder.AddListeningPort("0.0.0.0:51022", grpc::InsecureServerCredentials());
builder.RegisterService(&infoService);
builder.RegisterService(&projectManagerService);
// ...
mServer = builder.BuildAndStart();
mServer->Wait();  // blocks until shutdown
```

---

## 5. The Complete Mapping — One Method, End to End

Tracing `octaneVersion` from SDK header to client call:

```
┌─ SDK Header (apiinfo.h) ─────────────────────────────────────┐
│  class ApiInfo {                                              │
│      static int octaneVersion();                              │
│  };                                                           │
└───────────────────────────────────┬───────────────────────────┘
                                    │ machine-generated
┌─ Proto (apiinfo.proto) ───────────▼───────────────────────────┐
│  message ApiInfo {                                            │
│      message octaneVersionRequest {}                          │
│      message octaneVersionResponse { int32 result = 1; }      │
│  }                                                            │
│  service ApiInfoService {                                     │
│      rpc octaneVersion(Request) returns (Response);           │
│  }                                                            │
└───────────────────────────────────┬───────────────────────────┘
                                    │ protoc + grpc_cpp_plugin
┌─ Generated Stub (apiinfo.grpc.pb.h) ─▼───────────────────────┐
│  class ApiInfoService::Service {                              │
│      virtual Status octaneVersion(                            │
│          ServerContext*, const Request*, Response*);           │
│  };                                                           │
└───────────────────────────────────┬───────────────────────────┘
                                    │ hand-written override
┌─ Implementation (grpc_server.cpp) ▼───────────────────────────┐
│  class InfoServiceImpl : public ApiInfoService::Service {     │
│      Status octaneVersion(...) override {                     │
│          GRPC_SAFE_BEGIN(SVC)                                 │
│              response->set_result(                            │
│                  Octane::ApiInfo::octaneVersion());            │
│              return Status::OK;                               │
│          GRPC_SAFE_END(SVC)                                   │
│      }                                                        │
│  };                                                           │
└───────────────────────────────────────────────────────────────┘
```

The generated stub provides the exact method signature. The implementation is a thin bridge: unpack request → call SDK → pack response.

---

## 6. TypeScript Generation (octaneWebR)

The same `proto/` directory also generates TypeScript/JavaScript bindings for the browser UI and MCP server.

### Script: `octaneWebR/scripts/generate-proto.sh`

```bash
npx grpc_tools_node_protoc \
  --plugin=protoc-gen-ts=./node_modules/.bin/protoc-gen-ts \
  --ts_out=grpc_js:"$OUT_DIR" \
  --js_out=import_style=commonjs,binary:"$OUT_DIR" \
  --grpc_out=grpc_js:"$OUT_DIR" \
  -I "$PROTO_DIR" \
  "$PROTO_DIR"/*.proto
```

This produces:
- `*.d.ts` — TypeScript type definitions
- `*_pb.js` — message serialization (CommonJS)
- `*_grpc_pb.js` — client stubs

These are consumed by `OctaneGrpcClientBase.ts`, which is shared between the Vite dev server proxy (browser UI) and the MCP server (Claude Code integration).

---

## 7. Mapping the SDK to Any Interface

The 96 proto files are a **language-neutral interface definition** for the entire Octane SDK. Because gRPC has official codegen plugins for many languages, the same protos can generate native clients in any of them.

### What Already Exists

| Language   | Codegen Tool             | Status      | Used By                     |
|------------|--------------------------|-------------|-----------------------------|
| C++        | `protoc` + `grpc_cpp_plugin` | **Done** | octaneServGrpc (server)     |
| TypeScript | `grpc_tools_node_protoc` + `protoc-gen-ts` | **Done** | octaneWebR (browser + MCP) |

### Python

**Tool:** `grpcio-tools` (official Google package)

```bash
python -m grpc_tools.protoc \
  --proto_path=proto/ \
  --python_out=gen/ \
  --grpc_python_out=gen/ \
  proto/*.proto
```

Produces `*_pb2.py` (messages) and `*_pb2_grpc.py` (stubs). A Python client would look like:

```python
import grpc
from gen import apiinfo_pb2, apiinfo_pb2_grpc

channel = grpc.insecure_channel("localhost:51022")
stub = apiinfo_pb2_grpc.ApiInfoServiceStub(channel)
resp = stub.octaneVersion(apiinfo_pb2.ApiInfo.octaneVersionRequest())
print(resp.result)  # e.g. 202602
```

No server-side changes needed — the gRPC server already speaks the wire protocol.

### Lua

Lua has no official gRPC plugin, but there are two viable paths:

**Option A — gRPC-Lua binding (e.g. `grpc-lua`):**
```lua
local grpc = require("grpc")
local channel = grpc.channel("localhost:51022")
-- Call RPCs via dynamic proto loading
```

**Option B — Proto-to-Lua codegen + HTTP/2 client:**
Generate Lua message serializers from protos, use a Lua HTTP/2 library for the transport. More work, but no C dependency.

**Option C — FFI bridge to C++ client stubs:**
The C++ stubs already exist (`*.grpc.pb.h`). A thin LuaJIT FFI wrapper could call them directly. This is the approach used by many game engines that embed Lua.

### JavaScript (Browser / Deno / Bun)

**Already functional** via the TypeScript generation above. For browser-only (no Node.js gRPC), use `grpc-web`:

```bash
protoc \
  --plugin=protoc-gen-grpc-web=./protoc-gen-grpc-web \
  --grpc-web_out=import_style=typescript,mode=grpcwebtext:gen/ \
  proto/*.proto
```

Requires an Envoy or grpc-web proxy in front of the server (translates HTTP/1.1 to HTTP/2).

### The General Pattern

For **any** target language, the process is:

```
proto/*.proto (96 files, machine-generated from SDK headers)
       │
       │  protoc --{lang}_out + --grpc_{lang}_out
       ▼
  Generated client stubs in target language
       │
       │  gRPC channel → localhost:51022
       ▼
  octaneServGrpc.exe (unchanged)
       │
       │  C++ SDK calls
       ▼
  Octane Render SDK (octane.dll)
```

The server side never changes. Each new language binding is purely a client-side code generation step. The protos are the contract — they define every method signature, every field type, every enum value. A new language gets full Octane SDK access by running one `protoc` command.

### What a "Universal SDK" Would Look Like

```
                    ┌─────────────────────────┐
                    │  96 Proto Definitions    │
                    │  (source of truth)       │
                    └─────┬───────────────────┘
                          │
          ┌───────────────┼───────────────────┐
          │               │                   │
    protoc --cpp    protoc --python     protoc --ts
          │               │                   │
    ┌─────▼────┐   ┌──────▼─────┐    ┌───────▼──────┐
    │ C++ Stubs │   │ Python Stubs│    │ TS/JS Stubs  │
    │ (server)  │   │ (client)   │    │ (client)     │
    └──────────┘   └────────────┘    └──────────────┘
                          │                   │
                    octane-py SDK       octaneWebR
                    pip install         npm install
```

The protos already capture the full API surface. The missing piece for a true multi-language SDK is a thin **ergonomic wrapper** per language (handle lifecycle, error translation, Pythonic/Lua-ish naming). The gRPC stubs are correct but raw — a wrapper would add convenience like context managers, iterators, and idiomatic error handling.
