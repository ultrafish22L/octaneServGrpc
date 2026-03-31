#include "util/test_framework.h"
#include "util/connection.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace fuzz {

TestRunner::TestRunner(const std::string& addr, int timeoutMs, bool fullMode)
    : mAddr(addr), mTimeoutMs(timeoutMs), mFullMode(fullMode) {}

void TestRunner::addCategory(TestCategory cat) {
    mCategories.push_back(std::move(cat));
}

void TestRunner::listCategories() const {
    std::cout << "Available categories:\n";
    for (auto& c : mCategories)
        std::cout << "  " << c.name << " (" << c.tests.size() << " tests)\n";
}

bool TestRunner::healthCheck() {
    return isServerAlive(mAddr);
}

int TestRunner::execute(const std::string& filterCategory) {
    if (!healthCheck()) {
        std::cerr << "FATAL: Server not reachable at " << mAddr << "\n";
        return 1;
    }
    std::cout << "Server alive at " << mAddr << "\n\n";

    for (auto& cat : mCategories) {
        if (!filterCategory.empty() && cat.name != filterCategory)
            continue;
        runCategory(cat);
    }

    printSummary();
    return (mTotalFail + mTotalCrash + mTotalTimeout) > 0 ? 1 : 0;
}

void TestRunner::runCategory(const TestCategory& cat) {
    std::cout << "=== " << cat.name << " (" << cat.tests.size() << " tests) ===\n";

    for (auto& tc : cat.tests) {
        TestResult result;
        result.testName = tc.name;
        result.category = cat.name;
        result.expectedCode = tc.expectedCode;

        auto t0 = std::chrono::steady_clock::now();

        try {
            grpc::Status status = tc.run();
            auto t1 = std::chrono::steady_clock::now();
            result.elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            result.actualCode = status.error_code();

            if (status.error_code() == tc.expectedCode) {
                result.verdict = Verdict::PASS;
            } else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
                result.verdict = Verdict::TIMEOUT;
                result.detail = "RPC timed out";
            } else {
                result.verdict = Verdict::FAIL;
                std::ostringstream oss;
                oss << "expected " << static_cast<int>(tc.expectedCode)
                    << " got " << static_cast<int>(status.error_code());
                if (!status.error_message().empty())
                    oss << " (" << status.error_message().substr(0, 120) << ")";
                result.detail = oss.str();
            }
        } catch (const std::exception& e) {
            auto t1 = std::chrono::steady_clock::now();
            result.elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            result.verdict = Verdict::FAIL;
            result.detail = std::string("exception: ") + e.what();
        }

        // Print inline
        const char* tag = "";
        switch (result.verdict) {
            case Verdict::PASS:    tag = "PASS"; break;
            case Verdict::FAIL:    tag = "FAIL"; break;
            case Verdict::CRASH:   tag = "CRASH"; break;
            case Verdict::TIMEOUT: tag = "TIMEOUT"; break;
            case Verdict::SKIP:    tag = "SKIP"; break;
        }
        std::cout << "  [" << tag << "] " << tc.name;
        if (result.verdict != Verdict::PASS && !result.detail.empty())
            std::cout << " — " << result.detail;
        std::cout << " (" << std::fixed << std::setprecision(1)
                  << result.elapsedMs << "ms)\n";

        switch (result.verdict) {
            case Verdict::PASS:    mTotalPass++; break;
            case Verdict::FAIL:    mTotalFail++; break;
            case Verdict::CRASH:   mTotalCrash++; break;
            case Verdict::TIMEOUT: mTotalTimeout++; break;
            case Verdict::SKIP:    mTotalSkip++; break;
        }
        mResults.push_back(std::move(result));

        // Health check after failures
        if (result.verdict == Verdict::FAIL || result.verdict == Verdict::TIMEOUT) {
            if (!healthCheck()) {
                std::cerr << "  !! SERVER CRASHED after " << tc.name << " !!\n";
                // Mark remaining tests as CRASH
                // (skip — just break and let the category end)
                mTotalCrash++;
                break;
            }
        }
    }
    std::cout << "\n";
}

void TestRunner::printSummary() const {
    int total = mTotalPass + mTotalFail + mTotalCrash + mTotalTimeout + mTotalSkip;
    std::cout << "═══════════════════════════════════════\n"
              << "SUMMARY: " << total << " tests\n"
              << "  PASS:    " << mTotalPass << "\n"
              << "  FAIL:    " << mTotalFail << "\n"
              << "  CRASH:   " << mTotalCrash << "\n"
              << "  TIMEOUT: " << mTotalTimeout << "\n"
              << "  SKIP:    " << mTotalSkip << "\n"
              << "═══════════════════════════════════════\n";

    if (mTotalFail + mTotalCrash + mTotalTimeout > 0) {
        std::cout << "\nFailures:\n";
        for (auto& r : mResults) {
            if (r.verdict != Verdict::PASS && r.verdict != Verdict::SKIP)
                std::cout << "  " << r.category << "/" << r.testName
                          << " — " << r.detail << "\n";
        }
    }

    std::cout << "\nResult: " << (mTotalFail + mTotalCrash + mTotalTimeout == 0
                                    ? "ALL PASSED" : "FAILURES DETECTED") << "\n";
}

// Global helpers
std::unique_ptr<grpc::ClientContext> makeContext(int timeoutMs) {
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->set_deadline(std::chrono::system_clock::now()
                      + std::chrono::milliseconds(timeoutMs));
    return ctx;
}

std::shared_ptr<grpc::Channel> makeChannel(const std::string& addr) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(128 * 1024 * 1024); // 128MB for oversized tests
    args.SetMaxSendMessageSize(128 * 1024 * 1024);
    return grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);
}

} // namespace fuzz
