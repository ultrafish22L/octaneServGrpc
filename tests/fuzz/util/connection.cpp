#include "util/connection.h"
#include "util/test_framework.h"

namespace fuzz {

bool isServerAlive(const std::string& addr) {
    auto channel = makeChannel(addr);
    auto stub = livelinkapi::LiveLinkService::NewStub(channel);
    livelinkapi::Empty req;
    livelinkapi::ServVersionResponse resp;
    auto ctx = makeContext(3000);
    auto status = stub->GetServVersion(ctx.get(), req, &resp);
    return status.ok();
}

static std::shared_ptr<grpc::Channel> sChannel;
static std::string sChannelAddr;

std::shared_ptr<grpc::Channel> getSharedChannel(const std::string& addr) {
    if (!sChannel || sChannelAddr != addr) {
        sChannel = makeChannel(addr);
        sChannelAddr = addr;
    }
    return sChannel;
}

} // namespace fuzz
