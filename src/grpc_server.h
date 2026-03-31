#pragma once
// OctaneServGrpc Server — hosts all Octane gRPC services
//
// Creates a gRPC server on the specified port, registers service implementations,
// and runs until stopped.

#include <string>
#include <memory>
#include <atomic>
#include <cstdint>

namespace grpc { class Server; }

namespace OctaneServ {

class HandleRegistry;

class GrpcServer {
public:
    GrpcServer(uint16_t port = 51022);
    ~GrpcServer();

    /// Start the gRPC server. Blocks until StopServer() is called.
    void RunServer();

    /// Signal the server to stop.
    void StopServer();

    /// Get the port the server is listening on.
    uint16_t GetPort() const { return mPort; }

    /// True once the gRPC server is bound and accepting connections.
    bool IsRunning() const { return mRunning.load(); }

    /// Get the handle registry (shared across all services).
    HandleRegistry& GetHandleRegistry() { return *mHandleRegistry; }

private:
    uint16_t mPort;
    std::atomic<bool> mRunning{false};
    std::unique_ptr<grpc::Server> mServer;
    std::unique_ptr<HandleRegistry> mHandleRegistry;
};

} // namespace OctaneServ
