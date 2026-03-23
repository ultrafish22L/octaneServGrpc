#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Callback Dispatcher — bridges SDK callbacks to gRPC streams
//
// SDK callbacks fire on render threads. gRPC streams write on their own threads.
// This dispatcher is the thread-safe queue between them.
//
// Design:
//   SDK callback → enqueue event → condition_variable notify
//   gRPC stream  → wait on CV → dequeue → write to stream
//
// Multiple clients can subscribe (each gets their own copy of events).
// Queue has a max depth — oldest events are dropped if a slow client falls behind.
// ═══════════════════════════════════════════════════════════════════════════

#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <cstdint>
#include <functional>

namespace OctaneServ {

// Types of callback events the SDK can fire
enum class CallbackEventType {
    NewImage,           // Render frame ready
    NewStatistics,      // Render statistics updated
    RenderFailure,      // Render failed
    ProjectChanged,     // Project loaded/reset/created
    TileBlended,        // A tile was blended in the render film
};

// A single callback event. Lightweight — no image data stored here.
// For NewImage events, the gRPC stream handler calls grabRenderResult() itself.
struct CallbackEvent {
    CallbackEventType type;
    uint64_t userData = 0;
};

// Per-client subscription queue. Each connected gRPC stream gets one.
class CallbackSubscription {
public:
    explicit CallbackSubscription(size_t maxQueueDepth = 100)
        : mMaxDepth(maxQueueDepth) {}

    // Push an event (called from SDK callback thread). Thread-safe.
    void Push(const CallbackEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            // Drop oldest if queue full (render images are temporal — latest matters most)
            while (mQueue.size() >= mMaxDepth) {
                mQueue.pop();
            }
            mQueue.push(event);
        }
        mCV.notify_one();
    }

    // Wait for an event with timeout. Returns true if event was dequeued.
    // Returns false on timeout (check IsCancelled externally).
    bool WaitAndPop(CallbackEvent& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mCV.wait_for(lock, timeout, [this]{ return !mQueue.empty(); })) {
            out = mQueue.front();
            mQueue.pop();
            return true;
        }
        return false;
    }

    size_t QueueDepth() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mQueue.size();
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mCV;
    std::queue<CallbackEvent> mQueue;
    size_t mMaxDepth;
};

// Central dispatcher. Singleton. SDK callbacks push here, gRPC streams subscribe.
class CallbackDispatcher {
public:
    static CallbackDispatcher& Instance() {
        static CallbackDispatcher s;
        return s;
    }

    // Broadcast an event to all subscribers.
    void Broadcast(const CallbackEvent& event) {
        std::lock_guard<std::mutex> lock(mSubsMutex);
        for (auto* sub : mSubscriptions) {
            sub->Push(event);
        }
    }

    // Subscribe (called when a gRPC stream connects).
    void Subscribe(CallbackSubscription* sub) {
        std::lock_guard<std::mutex> lock(mSubsMutex);
        mSubscriptions.push_back(sub);
    }

    // Unsubscribe (called when a gRPC stream disconnects).
    void Unsubscribe(CallbackSubscription* sub) {
        std::lock_guard<std::mutex> lock(mSubsMutex);
        mSubscriptions.erase(
            std::remove(mSubscriptions.begin(), mSubscriptions.end(), sub),
            mSubscriptions.end());
    }

    // Number of active subscribers.
    size_t SubscriberCount() const {
        std::lock_guard<std::mutex> lock(mSubsMutex);
        return mSubscriptions.size();
    }

private:
    CallbackDispatcher() = default;
    mutable std::mutex mSubsMutex;
    std::vector<CallbackSubscription*> mSubscriptions;
};

} // namespace OctaneServ
