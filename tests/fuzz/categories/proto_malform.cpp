#include "categories/proto_malform.h"
#include "util/fuzz_values.h"
#include "util/connection.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/support/byte_buffer.h>

#include <condition_variable>
#include <mutex>

namespace fuzz {

// Send raw bytes to a gRPC method, bypassing proto serialization.
// Uses async PrepareUnaryCall + CompletionQueue for gRPC v1.62.1.
static grpc::Status sendRawBytes(const std::string& addr, int timeoutMs,
    const std::string& method, const std::string& payload) {
    auto channel = makeChannel(addr);
    grpc::GenericStub stub(channel);

    grpc::Slice slice(payload.data(), payload.size());
    grpc::ByteBuffer reqBuf(&slice, 1);
    grpc::ByteBuffer respBuf;

    grpc::CompletionQueue cq;
    auto ctx = makeContext(timeoutMs);

    auto rpc = stub.PrepareUnaryCall(ctx.get(), method, reqBuf, &cq);
    rpc->StartCall();

    grpc::Status status;
    rpc->Finish(&respBuf, &status, reinterpret_cast<void*>(1));

    void* tag;
    bool ok;
    cq.Next(&tag, &ok);

    return status;
}

TestCategory makeProtoMalform(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "proto_malform";

    const std::string method = "/octaneapi.ApiInfoService/octaneVersion";

    // Empty payload — valid for parameterless RPCs
    cat.tests.push_back({"empty_payload", "empty bytes to octaneVersion (valid for empty request)",
        grpc::StatusCode::OK,
        [addr, timeoutMs, method]() {
            return sendRawBytes(addr, timeoutMs, method, "");
        }
    });

    // Truncated varint — field 1, varint type, but no following byte
    cat.tests.push_back({"truncated_varint", "truncated varint (0x08 alone)",
        grpc::StatusCode::INTERNAL,
        [addr, timeoutMs, method]() {
            return sendRawBytes(addr, timeoutMs, method, std::string("\x08", 1));
        }
    });

    // Wrong wire type — field 1 as 64-bit fixed (type 1) instead of varint (type 0)
    // Proto3 may silently skip unknown wire types → OK
    cat.tests.push_back({"wrong_wire_type",
        "field 1 as 64-bit fixed instead of varint",
        grpc::StatusCode::OK,
        [addr, timeoutMs, method]() {
            // Wire type 1 = 64-bit, field 1 → tag = (1 << 3) | 1 = 0x09
            std::string payload("\x09\x00\x00\x00\x00\x00\x00\x00\x00", 9);
            return sendRawBytes(addr, timeoutMs, method, payload);
        }
    });

    // Unknown field 999 — proto3 should silently ignore
    cat.tests.push_back({"unknown_field_999",
        "unknown field 999 (proto3 ignores unknown fields)",
        grpc::StatusCode::OK,
        [addr, timeoutMs, method]() {
            // Field 999, varint type → tag = (999 << 3) | 0 = 7992 → varint encoding
            // 7992 = 0x1F38 → varint: 0xB8 0x3E
            std::string payload("\xb8\x3e\x42", 3); // field 999 = 66
            return sendRawBytes(addr, timeoutMs, method, payload);
        }
    });

    // 256 bytes of random garbage
    cat.tests.push_back({"random_garbage_256",
        "256 bytes random garbage",
        grpc::StatusCode::INTERNAL,
        [addr, timeoutMs, method]() {
            return sendRawBytes(addr, timeoutMs, method, randomBytes(256));
        }
    });

    // Valid request bytes sent to wrong method
    cat.tests.push_back({"valid_to_wrong_method",
        "empty request sent to non-existent method",
        grpc::StatusCode::UNIMPLEMENTED,
        [addr, timeoutMs]() {
            return sendRawBytes(addr, timeoutMs,
                "/octaneapi.ApiInfoService/nonExistentMethod", "");
        }
    });

    return cat;
}

} // namespace fuzz
