# Testing

## Smoke Test

After building, verify the server works end-to-end:

```bash
# Start server
build/Release/octaneServGrpc.exe

# In another terminal (or via MCP tools):
get_octane_version          # handshake — verify serv_build
reset_project               # clean scene
place_geo(sphere)           # create geometry
fit_camera                  # frame it
start_render                # render
save_render                 # save PNG and verify visually
```

## MCP Test Suite

Full test coverage runs through the MCP layer. See `octaneWebR/docs/mcp/TESTING.md` for the test plan, categories (A-M), and pass criteria.

Every test follows: Act → Log → Render → Pass/Fail → Next.

## Validation Testing

The server returns descriptive errors for all invalid inputs. Test by sending bad values:

| Test | Expected Error |
|------|---------------|
| `set_attribute` with wrong type | `INVALID_ARGUMENT: type mismatch on attribute...` |
| `connect_nodes` with incompatible pin | `WRN: connection did not take effect...` |
| `start_render` with no geometry | `FAILED_PRECONDITION: pin 3 (geometry/mesh) has no node connected` |
| Handle 0 or stale handle | `NOT_FOUND: handle not found in registry` |
| `setClayMode(99)` | `INVALID_ARGUMENT: clay mode value 99 is not valid` |

## Log Verification

After any test, check `build/Release/log_serv.log` for clean REQ/RES patterns. No unexpected ERR lines = pass.
