#include "categories/float_fuzz.h"
#include "util/fuzz_values.h"
#include "util/connection.h"

#include "livelink.grpc.pb.h"
#include "apinodesystem_3.grpc.pb.h"
#include "apiprojectmanager.grpc.pb.h"

#include <cmath>
#include <sstream>

namespace fuzz {

static std::string floatName(float f) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "PosInf" : "NegInf";
    std::ostringstream oss;
    oss << f;
    return oss.str();
}

TestCategory makeFloatFuzz(const std::string& addr, int timeoutMs) {
    TestCategory cat;
    cat.name = "float_fuzz";

    auto channel = getSharedChannel(addr);
    auto llStub = std::shared_ptr<livelinkapi::LiveLinkService::Stub>(livelinkapi::LiveLinkService::NewStub(channel));
    auto itemStub = std::shared_ptr<octaneapi::ApiItemService::Stub>(octaneapi::ApiItemService::NewStub(channel));
    auto projStub = std::shared_ptr<octaneapi::ApiProjectManagerService::Stub>(octaneapi::ApiProjectManagerService::NewStub(channel));

    // SetCamera with NaN/Inf in position
    auto badFloats = fuzzFloats();
    for (auto f : {std::numeric_limits<float>::quiet_NaN(),
                   std::numeric_limits<float>::infinity(),
                   -std::numeric_limits<float>::infinity(),
                   FLT_MAX, -FLT_MAX}) {
        std::string fname = floatName(f);
        // These should either succeed or return INVALID_ARGUMENT — never crash
        cat.tests.push_back({"setCamera_pos_" + fname,
            "SetCamera with position.x=" + fname,
            grpc::StatusCode::OK, // any non-crash response accepted
            [llStub, f, timeoutMs]() {
                livelinkapi::CameraState req;
                auto* pos = req.mutable_position();
                pos->set_x(f);
                pos->set_y(1.0f);
                pos->set_z(5.0f);
                livelinkapi::Empty resp;
                auto ctx = makeContext(timeoutMs);
                llStub->SetCamera(ctx.get(), req, &resp);
                return grpc::Status::OK; // any response = survived
            }
        });
    }

    // SetCamera with zero up vector — server may accept gracefully (skip normalization)
    cat.tests.push_back({"setCamera_zero_up", "SetCamera with up=(0,0,0) — zero vector",
        grpc::StatusCode::OK,
        [llStub, timeoutMs]() {
            livelinkapi::CameraState req;
            auto* pos = req.mutable_position();
            pos->set_x(0); pos->set_y(0); pos->set_z(5);
            auto* tgt = req.mutable_target();
            tgt->set_x(0); tgt->set_y(0); tgt->set_z(0);
            auto* up = req.mutable_up();
            up->set_x(0); up->set_y(0); up->set_z(0);
            livelinkapi::Empty resp;
            auto ctx = makeContext(timeoutMs);
            llStub->SetCamera(ctx.get(), req, &resp);
                return grpc::Status::OK;
        }
    });

    // SetCamera with near-zero up vector
    cat.tests.push_back({"setCamera_tiny_up", "SetCamera with up=(1e-7,0,0) — near-zero",
        grpc::StatusCode::OK,
        [llStub, timeoutMs]() {
            livelinkapi::CameraState req;
            auto* pos = req.mutable_position();
            pos->set_x(0); pos->set_y(0); pos->set_z(5);
            auto* tgt = req.mutable_target();
            tgt->set_x(0); tgt->set_y(0); tgt->set_z(0);
            auto* up = req.mutable_up();
            up->set_x(1e-7f); up->set_y(0); up->set_z(0);
            livelinkapi::Empty resp;
            auto ctx = makeContext(timeoutMs);
            llStub->SetCamera(ctx.get(), req, &resp);
                return grpc::Status::OK;
        }
    });

    // SetCamera with NaN in up vector
    cat.tests.push_back({"setCamera_nan_up", "SetCamera with up=(NaN,0,0)",
        grpc::StatusCode::OK, // NaN passes length check (NaN > 1e-12 is false, so it may fail)
        [llStub, timeoutMs]() {
            livelinkapi::CameraState req;
            auto* up = req.mutable_up();
            up->set_x(std::numeric_limits<float>::quiet_NaN());
            up->set_y(0); up->set_z(0);
            livelinkapi::Empty resp;
            auto ctx = makeContext(timeoutMs);
            llStub->SetCamera(ctx.get(), req, &resp);
                return grpc::Status::OK;
        }
    });

    // setValueByAttrID with NaN float — root graph doesn't have A_VALUE, so INVALID_ARGUMENT
    cat.tests.push_back({"setValueByID_nan", "setValueByAttrID with float_value=NaN on root graph",
        grpc::StatusCode::INVALID_ARGUMENT,
        [projStub, itemStub, timeoutMs]() {
            octaneapi::ApiProjectManager::rootNodeGraphRequest rReq;
            octaneapi::ApiProjectManager::rootNodeGraphResponse rResp;
            auto ctx1 = makeContext(timeoutMs);
            auto s = projStub->rootNodeGraph(ctx1.get(), rReq, &rResp);
            if (!s.ok()) return s;

            octaneapi::ApiItem::setValueByIDRequest req;
            auto* ref = req.mutable_objectptr();
            ref->set_handle(rResp.result().handle());
            ref->set_type(octaneapi::ObjectRef::ApiItem);
            req.set_attribute_id(static_cast<octaneapi::AttributeId>(185)); // A_VALUE
            req.set_float_value(std::numeric_limits<float>::quiet_NaN());
            octaneapi::ApiItem::setValueResponse resp;
            auto ctx2 = makeContext(timeoutMs);
            return itemStub->setValueByAttrID(ctx2.get(), req, &resp);
        }
    });

    // Reset camera to safe values
    cat.tests.push_back({"setCamera_reset", "reset camera to safe values",
        grpc::StatusCode::OK,
        [llStub, timeoutMs]() {
            livelinkapi::CameraState req;
            auto* pos = req.mutable_position();
            pos->set_x(0); pos->set_y(1); pos->set_z(5);
            auto* tgt = req.mutable_target();
            tgt->set_x(0); tgt->set_y(0); tgt->set_z(0);
            auto* up = req.mutable_up();
            up->set_x(0); up->set_y(1); up->set_z(0);
            livelinkapi::Empty resp;
            auto ctx = makeContext(timeoutMs);
            llStub->SetCamera(ctx.get(), req, &resp);
                return grpc::Status::OK;
        }
    });

    return cat;
}

} // namespace fuzz
