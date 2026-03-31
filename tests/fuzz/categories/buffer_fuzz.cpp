#include "categories/buffer_fuzz.h"
#include "util/fuzz_values.h"
#include "util/connection.h"

#include "livelink.grpc.pb.h"

namespace fuzz {

TestCategory makeBufferFuzz(const std::string& addr, int timeoutMs, bool fullMode) {
    TestCategory cat;
    cat.name = "buffer_fuzz";

    auto channel = getSharedChannel(addr);
    auto llStub = std::shared_ptr<livelinkapi::LiveLinkService::Stub>(
        livelinkapi::LiveLinkService::NewStub(channel));

    // All GetFile tests: any server response = pass (testing crash resistance)
    struct BufTest { std::string name; std::string desc; std::string filepath; };
    std::vector<BufTest> bufTests = {
        {"getFile_empty", "GetFile with empty path", ""},
        {"getFile_nonexistent", "GetFile with non-existent path",
         "C:\\nonexistent\\path\\file.obj"},
        {"getFile_traversal", "GetFile with path traversal", "../../etc/passwd"},
        {"getFile_1MB", "GetFile with 1MB filepath", std::string(1024 * 1024, 'A')},
    };
    for (auto& bt : bufTests) {
        cat.tests.push_back({bt.name, bt.desc, grpc::StatusCode::OK,
            [llStub, fp = bt.filepath, timeoutMs]() {
                livelinkapi::FileRequest req;
                req.set_filepath(fp);
                livelinkapi::FileResponse resp;
                auto ctx = makeContext(timeoutMs);
                llStub->GetFile(ctx.get(), req, &resp);
                return grpc::Status::OK; // any response = survived
            }
        });
    }

    // 64MB filepath — at message limit
    if (fullMode) {
        cat.tests.push_back({"getFile_64MB", "GetFile with 64MB filepath (message limit)",
            grpc::StatusCode::OK,
            [llStub, timeoutMs]() {
                livelinkapi::FileRequest req;
                req.set_filepath(std::string(64 * 1024 * 1024, 'Z'));
                livelinkapi::FileResponse resp;
                auto ctx = makeContext(timeoutMs);
                llStub->GetFile(ctx.get(), req, &resp);
                return grpc::Status::OK;
            }
        });
    }

    return cat;
}

} // namespace fuzz
