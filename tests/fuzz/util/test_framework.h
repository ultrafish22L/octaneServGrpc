#pragma once
// Test framework for gRPC fuzz testing.
// Manages test registration, execution with timeouts, crash detection, and reporting.

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace fuzz {

enum class Verdict { PASS, FAIL, CRASH, TIMEOUT, SKIP };

struct TestResult {
    std::string testName;
    std::string category;
    Verdict     verdict = Verdict::FAIL;
    std::string detail;
    grpc::StatusCode actualCode   = grpc::StatusCode::OK;
    grpc::StatusCode expectedCode = grpc::StatusCode::OK;
    double      elapsedMs = 0.0;
};

struct TestCase {
    std::string name;
    std::string description;
    grpc::StatusCode expectedCode = grpc::StatusCode::OK;
    std::function<grpc::Status()> run;
};

struct TestCategory {
    std::string name;
    std::vector<TestCase> tests;
};

class TestRunner {
public:
    TestRunner(const std::string& addr, int timeoutMs, bool fullMode);

    void addCategory(TestCategory cat);
    int  execute(const std::string& filterCategory = "");
    void listCategories() const;

    const std::string& addr() const { return mAddr; }
    int timeoutMs() const { return mTimeoutMs; }
    bool fullMode() const { return mFullMode; }

private:
    bool healthCheck();
    void runCategory(const TestCategory& cat);
    void printSummary() const;

    std::string mAddr;
    int  mTimeoutMs;
    bool mFullMode;

    std::vector<TestCategory> mCategories;
    std::vector<TestResult>   mResults;
    std::mutex                mResultsMtx;

    int mTotalPass  = 0;
    int mTotalFail  = 0;
    int mTotalCrash = 0;
    int mTotalTimeout = 0;
    int mTotalSkip  = 0;
};

// Helpers for test code
std::unique_ptr<grpc::ClientContext> makeContext(int timeoutMs);
std::shared_ptr<grpc::Channel> makeChannel(const std::string& addr);

} // namespace fuzz
