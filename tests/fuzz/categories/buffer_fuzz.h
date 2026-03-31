#pragma once
#include "util/test_framework.h"
namespace fuzz { TestCategory makeBufferFuzz(const std::string& addr, int timeoutMs, bool fullMode); }
