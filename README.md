# octaneServGrpc

C++ gRPC server embedding Octane Render SDK 2026.2. No separate octane.exe — the server IS the render engine.

- Links `octane.dll` directly (GPU rendering, scene graph, ORBX, everything)
- 96 Octane proto services on port 51022
- octaneWebR + MCP connect with zero changes

See [QUICKSTART.md](QUICKSTART.md) to build and run. See [docs/](docs/) for architecture, reference, troubleshooting, and design docs.

## Status

Full scene building, rendering, and viewport streaming working. All RPCs hardened with GRPC_SAFE exception handling. Verified with glass metal DRESS test (3-sphere scene end-to-end via MCP).

**Not yet implemented:** Full ItemArray iteration, pick intersection, handle staling validation (see `docs/PLAN.md`).

Requires valid Octane subscription (PLORTEST plugin key). Falls back to demo mode without activation.
