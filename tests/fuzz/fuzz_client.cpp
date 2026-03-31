// octane_fuzz_client — comprehensive fuzz & stress test client for octaneServGrpc
//
// Usage:
//   octane_fuzz_client [--addr HOST:PORT] [--category NAME] [--timeout MS] [--full] [--list]

#include "util/test_framework.h"

#include <iostream>
#include <string>

// Category registration functions
namespace fuzz {
    TestCategory makeHandleFuzz(const std::string& addr, int timeoutMs);
    TestCategory makeTypeConfusion(const std::string& addr, int timeoutMs);
    TestCategory makeEnumFuzz(const std::string& addr, int timeoutMs);
    TestCategory makeNumericBoundary(const std::string& addr, int timeoutMs);
    TestCategory makeFloatFuzz(const std::string& addr, int timeoutMs);
    TestCategory makeStringFuzz(const std::string& addr, int timeoutMs, bool fullMode);
    TestCategory makeBufferFuzz(const std::string& addr, int timeoutMs, bool fullMode);
    TestCategory makeProtoMalform(const std::string& addr, int timeoutMs);
    TestCategory makeRapidFire(const std::string& addr, int timeoutMs);
    TestCategory makeConcurrentStress(const std::string& addr, int timeoutMs);
    TestCategory makeConnectionAbuse(const std::string& addr, int timeoutMs);
    TestCategory makeStreamAbuse(const std::string& addr, int timeoutMs);
    TestCategory makeStaleHandle(const std::string& addr, int timeoutMs);
    TestCategory makeStateMachine(const std::string& addr, int timeoutMs);
    TestCategory makeResourceExhaust(const std::string& addr, int timeoutMs);
    TestCategory makeSequenceFuzz(const std::string& addr, int timeoutMs);
}

int main(int argc, char* argv[]) {
    std::string addr = "localhost:51022";
    std::string category;
    int timeoutMs = 5000;
    bool fullMode = false;
    bool listMode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") {
            listMode = true;
        } else if (arg == "--full") {
            fullMode = true;
        } else if (arg == "--addr" && i + 1 < argc) {
            addr = argv[++i];
        } else if (arg == "--category" && i + 1 < argc) {
            category = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeoutMs = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: octane_fuzz_client [OPTIONS]\n"
                      << "  --addr HOST:PORT   Server address (default: localhost:51022)\n"
                      << "  --category NAME    Run only this category\n"
                      << "  --timeout MS       Per-RPC timeout (default: 5000)\n"
                      << "  --full             Include 64MB message tests\n"
                      << "  --list             List available categories\n";
            return 0;
        }
    }

    fuzz::TestRunner runner(addr, timeoutMs, fullMode);

    // Register all categories
    runner.addCategory(fuzz::makeHandleFuzz(addr, timeoutMs));
    runner.addCategory(fuzz::makeTypeConfusion(addr, timeoutMs));
    runner.addCategory(fuzz::makeEnumFuzz(addr, timeoutMs));
    runner.addCategory(fuzz::makeNumericBoundary(addr, timeoutMs));
    runner.addCategory(fuzz::makeFloatFuzz(addr, timeoutMs));
    runner.addCategory(fuzz::makeStringFuzz(addr, timeoutMs, fullMode));
    runner.addCategory(fuzz::makeBufferFuzz(addr, timeoutMs, fullMode));
    runner.addCategory(fuzz::makeProtoMalform(addr, timeoutMs));
    runner.addCategory(fuzz::makeRapidFire(addr, timeoutMs));
    runner.addCategory(fuzz::makeConcurrentStress(addr, timeoutMs));
    runner.addCategory(fuzz::makeConnectionAbuse(addr, timeoutMs));
    runner.addCategory(fuzz::makeStreamAbuse(addr, timeoutMs));
    runner.addCategory(fuzz::makeStaleHandle(addr, timeoutMs));
    runner.addCategory(fuzz::makeStateMachine(addr, timeoutMs));
    runner.addCategory(fuzz::makeResourceExhaust(addr, timeoutMs));
    runner.addCategory(fuzz::makeSequenceFuzz(addr, timeoutMs));

    if (listMode) {
        runner.listCategories();
        return 0;
    }

    std::cout << "octane_fuzz_client — gRPC fuzz & stress test\n"
              << "Target: " << addr << " | Timeout: " << timeoutMs
              << "ms | Full: " << (fullMode ? "yes" : "no") << "\n\n";

    return runner.execute(category);
}
