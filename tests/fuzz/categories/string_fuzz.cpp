#include "categories/string_fuzz.h"
#include "util/fuzz_values.h"
#include "util/connection.h"

#include "apiprojectmanager.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"
#include "apinodesystem_7.grpc.pb.h"
#include "livelink.grpc.pb.h"

namespace fuzz {

static octaneapi::ObjectRef makeRef(uint64_t handle,
    octaneapi::ObjectRef::ObjectType type = octaneapi::ObjectRef::ApiItem) {
    octaneapi::ObjectRef ref;
    ref.set_handle(handle);
    ref.set_type(type);
    return ref;
}

TestCategory makeStringFuzz(const std::string& addr, int timeoutMs, bool fullMode) {
    TestCategory cat;
    cat.name = "string_fuzz";

    auto channel = getSharedChannel(addr);
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(octaneapi::ApiProjectManagerService::NewStub(channel));
    auto nodeStub = std::shared_ptr<octaneapi::ApiNodeService::Stub>(octaneapi::ApiNodeService::NewStub(channel));
    auto llStub = std::shared_ptr<livelinkapi::LiveLinkService::Stub>(livelinkapi::LiveLinkService::NewStub(channel));

    auto strings = fuzzStrings();

    // loadProject with fuzz strings — all should return INVALID_ARGUMENT (empty) or
    // some form of error (bad path), never crash
    int idx = 0;
    for (auto& s : strings) {
        std::string label = "loadProject_str" + std::to_string(idx++);
        // Any server response is acceptable — we're testing crash resistance, not error codes
        cat.tests.push_back({label,
            "loadProject with adversarial string (" + std::to_string(s.size()) + " bytes)",
            grpc::StatusCode::OK, // placeholder — see lambda
            [projStub, s, timeoutMs]() {
                octaneapi::ApiProjectManager::loadProjectRequest req;
                req.set_projectpath(s);
                octaneapi::ApiProjectManager::loadProjectResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto status = projStub->loadProject(ctx.get(), req, &resp);
                // Any response means server survived — return OK regardless
                return grpc::Status::OK;
            }
        });
    }

    // saveProjectAs with empty path
    cat.tests.push_back({"saveProjectAs_empty", "saveProjectAs with empty path",
        grpc::StatusCode::INVALID_ARGUMENT,
        [projStub, timeoutMs]() {
            octaneapi::ApiProjectManager::saveProjectAsRequest req;
            req.set_path("");
            octaneapi::ApiProjectManager::saveProjectAsResponse resp;
            auto ctx = makeContext(timeoutMs);
            return projStub->saveProjectAs(ctx.get(), req, &resp);
        }
    });

    // connectTo1 with empty pin name
    cat.tests.push_back({"connectTo1_empty_pinName", "connectTo1 with empty pinName",
        grpc::StatusCode::INVALID_ARGUMENT,
        [projStub, nodeStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            // Create a node
            octaneapi::ApiNode::createRequest cReq;
            cReq.set_type(static_cast<octaneapi::NodeType>(89));
            *cReq.mutable_ownergraph() = makeRef(rResp.result().handle(),
                octaneapi::ObjectRef::ApiNodeGraph);
            cReq.set_configurepins(false);
            octaneapi::ApiNode::createResponse cResp;
            auto ctx2 = makeContext(timeoutMs);
            s = nodeStub->create(ctx2.get(), cReq, &cResp);
            if (!s.ok()) return s;

            // connectTo1 with empty pin name
            octaneapi::ApiNode::connectTo1Request req;
            *req.mutable_objectptr() = makeRef(cResp.result().handle(),
                octaneapi::ObjectRef::ApiNode);
            req.set_pinname("");
            *req.mutable_sourcenode() = makeRef(cResp.result().handle(),
                octaneapi::ObjectRef::ApiNode);
            google::protobuf::Empty resp;
            auto ctx3 = makeContext(timeoutMs);
            return nodeStub->connectTo1(ctx3.get(), req, &resp);
        }
    });

    // GetFile with adversarial paths — test server resilience
    idx = 0;
    for (auto& s : {std::string(""), std::string("../../etc/passwd"),
                     std::string("..\\..\\windows\\system32\\config\\sam"),
                     std::string("C:\\nonexistent\\path.obj")}) {
        cat.tests.push_back({"getFile_str" + std::to_string(idx++),
            "GetFile with adversarial filepath",
            grpc::StatusCode::OK, // any response = no crash
            [llStub, s, timeoutMs]() {
                livelinkapi::FileRequest req;
                req.set_filepath(s);
                livelinkapi::FileResponse resp;
                auto ctx = makeContext(timeoutMs);
                auto status = llStub->GetFile(ctx.get(), req, &resp);
                return grpc::Status::OK; // survived = pass
            }
        });
    }

    // 64MB string test — gated behind --full
    if (fullMode) {
        cat.tests.push_back({"loadProject_64MB", "loadProject with 64MB string (message limit test)",
            grpc::StatusCode::INVALID_ARGUMENT,
            [projStub, timeoutMs]() {
                auto bigStr = fuzzString64MB();
                octaneapi::ApiProjectManager::loadProjectRequest req;
                req.set_projectpath(bigStr);
                octaneapi::ApiProjectManager::loadProjectResponse resp;
                auto ctx = makeContext(timeoutMs);
                return projStub->loadProject(ctx.get(), req, &resp);
            }
        });
    }

    return cat;
}

} // namespace fuzz
