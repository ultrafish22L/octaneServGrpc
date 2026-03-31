#pragma once
// Server connection management and health checking.

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "livelink.grpc.pb.h"

namespace fuzz {

// Check if server is alive via GetServVersion
bool isServerAlive(const std::string& addr);

// Shared channel for the test session
std::shared_ptr<grpc::Channel> getSharedChannel(const std::string& addr);

} // namespace fuzz
