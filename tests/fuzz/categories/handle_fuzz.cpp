#include "categories/handle_fuzz.h"
#include "util/fuzz_values.h"
#include "util/connection.h"

#include "apinodesystem_3.grpc.pb.h"
#include "apinodesystem_1.grpc.pb.h"
#include "apinodesystem_7.grpc.pb.h"

namespace fuzz {

static octaneapi::ObjectRef makeRef(uint64_t handle) {
    octaneapi::ObjectRef ref;
    ref.set_handle(handle);
    ref.set_type(octaneapi::ObjectRef::ApiItem);
    return ref;
}

TestCategory makeHandleFuzz(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "handle_fuzz";

    auto channel = getSharedChannel(addr);
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(octaneapi::ApiItemService::NewStub(channel));
    auto nodeStub = std::shared_ptr<octaneapi::ApiNodeService::Stub>(octaneapi::ApiNodeService::NewStub(channel));
    auto arrayStub = std::shared_ptr<octaneapi::ApiItemArrayService::Stub>(octaneapi::ApiItemArrayService::NewStub(channel));

    auto handles = fuzzHandles();

    // isNode with various bad handles
    for (auto h : handles) {
        auto expected = (h == 0) ? grpc::StatusCode::INVALID_ARGUMENT
                                 : grpc::StatusCode::NOT_FOUND;
        std::string name = "isNode_handle_" + std::to_string(h);

        cat.tests.push_back({name, "isNode with handle=" + std::to_string(h), expected,
            [itemStub, h, timeoutMs]() {
                octaneapi::ApiItem::isNodeRequest req;
                *req.mutable_objectptr() = makeRef(h);
                octaneapi::ApiItem::isNodeResponse resp;
                auto ctx = makeContext(timeoutMs);
                return itemStub->isNode(ctx.get(), req, &resp);
            }
        });
    }

    // name with handle=0
    cat.tests.push_back({"name_handle_0", "name with handle=0",
        grpc::StatusCode::INVALID_ARGUMENT,
        [itemStub, timeoutMs]() {
            octaneapi::ApiItem::nameRequest req;
            *req.mutable_objectptr() = makeRef(0);
            octaneapi::ApiItem::nameResponse resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->name(ctx.get(), req, &resp);
        }
    });

    // destroy with handle=0
    cat.tests.push_back({"destroy_handle_0", "destroy with handle=0",
        grpc::StatusCode::INVALID_ARGUMENT,
        [itemStub, timeoutMs]() {
            octaneapi::ApiItem::destroyRequest req;
            *req.mutable_objectptr() = makeRef(0);
            google::protobuf::Empty resp;
            auto ctx = makeContext(timeoutMs);
            return itemStub->destroy(ctx.get(), req, &resp);
        }
    });

    // pinCount with handle=0
    cat.tests.push_back({"pinCount_handle_0", "pinCount with handle=0",
        grpc::StatusCode::INVALID_ARGUMENT,
        [nodeStub, timeoutMs]() {
            octaneapi::ApiNode::pinCountRequest req;
            *req.mutable_objectptr() = makeRef(0);
            octaneapi::ApiNode::pinCountResponse resp;
            auto ctx = makeContext(timeoutMs);
            return nodeStub->pinCount(ctx.get(), req, &resp);
        }
    });

    // array size with handle=0
    cat.tests.push_back({"array_size_handle_0", "array.size with handle=0",
        grpc::StatusCode::INVALID_ARGUMENT,
        [arrayStub, timeoutMs]() {
            octaneapi::ApiItemArray::sizeRequest req;
            *req.mutable_objectptr() = makeRef(0);
            octaneapi::ApiItemArray::sizeResponse resp;
            auto ctx = makeContext(timeoutMs);
            return arrayStub->size(ctx.get(), req, &resp);
        }
    });

    // array get with handle=DEADBEEF
    cat.tests.push_back({"array_get_handle_deadbeef", "array.get with fake handle",
        grpc::StatusCode::NOT_FOUND,
        [arrayStub, timeoutMs]() {
            octaneapi::ApiItemArray::getRequest req;
            *req.mutable_objectptr() = makeRef(0xDEADBEEFDEADBEEFULL);
            req.set_index(0);
            octaneapi::ApiItemArray::getResponse resp;
            auto ctx = makeContext(timeoutMs);
            return arrayStub->get(ctx.get(), req, &resp);
        }
    });

    return cat;
}

} // namespace fuzz
