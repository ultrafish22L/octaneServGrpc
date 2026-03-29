#include "handle_registry.h"
#include "octaneapi.h"

namespace OctaneServ {

uint64_t HandleRegistry::Register(Octane::ApiItem* item) {
    if (!item) return 0;
    uint64_t handle = static_cast<uint64_t>(item->uniqueId());
    if (handle == 0) return 0; // Invalid item (SDK says uniqueId 0 = invalid)
    std::lock_guard<std::mutex> lock(mMutex);
    mHandles[handle] = item;
    return handle;
}

Octane::ApiItem* HandleRegistry::Lookup(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mHandles.find(handle);
    if (it == mHandles.end()) return nullptr;

    // Staleness validation: if the item's uniqueId is now 0, it's been destroyed.
    // Auto-evict and return nullptr.
    Octane::ApiItem* item = it->second;
    if (!item || item->uniqueId() == 0) {
        mHandles.erase(it);
        ++mStaleEvictions;
        return nullptr;
    }
    return item;
}

void HandleRegistry::Unregister(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mMutex);
    mHandles.erase(handle);
}

uint64_t HandleRegistry::RegisterArray(Octane::ApiItemArray* arr) {
    if (!arr) return 0;
    std::lock_guard<std::mutex> lock(mMutex);
    uint64_t handle = mNextArrayHandle++;
    mArrays[handle] = std::unique_ptr<Octane::ApiItemArray>(arr);
    return handle;
}

Octane::ApiItemArray* HandleRegistry::LookupArray(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mArrays.find(handle);
    return (it != mArrays.end()) ? it->second.get() : nullptr;
}

void HandleRegistry::UnregisterArray(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mMutex);
    mArrays.erase(handle);
}

void HandleRegistry::Clear() {
    std::lock_guard<std::mutex> lock(mMutex);
    mHandles.clear();
    mArrays.clear();
    mNextArrayHandle = 0x0010000000000000ULL; // 2^52, JS-safe
}

size_t HandleRegistry::Size() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mHandles.size() + mArrays.size();
}

size_t HandleRegistry::ItemCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mHandles.size();
}

size_t HandleRegistry::ArrayCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mArrays.size();
}

uint64_t HandleRegistry::StaleEvictions() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mStaleEvictions;
}

} // namespace OctaneServ
