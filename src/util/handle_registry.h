#pragma once
// Handle Registry — maps gRPC uint64 handles to SDK objects
//
// HARDENED: Validates pointers on every lookup. Auto-evicts stale entries.
//
// Two types of handles:
// 1. ApiItem* (nodes, graphs) — uses SDK uniqueId() as handle
//    Validated on lookup: if uniqueId() returns 0, the item is dead.
// 2. ApiItemArray* (owned, heap-allocated) — uses synthetic handles (high range)
//    These are short-lived: allocated during a call, used briefly, then freed.

#include <cstdint>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace Octane { class ApiItem; class ApiItemArray; }

namespace OctaneServ {

class HandleRegistry {
public:
    /// Register an SDK item and return its handle (uniqueId).
    /// Returns 0 if item is null or has uniqueId 0.
    uint64_t Register(Octane::ApiItem* item);

    /// Look up an SDK item by handle. Returns nullptr if not found or stale.
    /// Validates the pointer is still alive (uniqueId != 0). Auto-evicts stale entries.
    Octane::ApiItem* Lookup(uint64_t handle);

    /// Remove an item from the registry.
    void Unregister(uint64_t handle);

    /// Register a heap-allocated ApiItemArray and return its handle.
    /// Registry takes ownership and will delete it on Clear/Unregister.
    uint64_t RegisterArray(Octane::ApiItemArray* arr);

    /// Look up an ApiItemArray by handle. Returns nullptr if not found.
    Octane::ApiItemArray* LookupArray(uint64_t handle);

    /// Unregister and delete an ApiItemArray.
    void UnregisterArray(uint64_t handle);

    /// Clear all registered handles (items + arrays).
    /// Called on loadProject/resetProject to prevent stale pointers.
    void Clear();

    /// Get the number of registered handles (items + arrays).
    size_t Size() const;

    /// Get counts for diagnostics.
    size_t ItemCount() const;
    size_t ArrayCount() const;
    uint64_t StaleEvictions() const;

private:
    mutable std::mutex mMutex;
    std::unordered_map<uint64_t, Octane::ApiItem*> mHandles;
    std::unordered_map<uint64_t, std::unique_ptr<Octane::ApiItemArray>> mArrays;
    // Array handles must stay within JS Number.MAX_SAFE_INTEGER (2^53 - 1)
    // to avoid precision loss when round-tripping through JSON/protobuf-js.
    // Use 2^52 as the base — well above any realistic SDK uniqueId.
    uint64_t mNextArrayHandle = 0x0010000000000000ULL; // 2^52, JS-safe
    uint64_t mStaleEvictions = 0; // Counter for diagnostics
};

} // namespace OctaneServ
