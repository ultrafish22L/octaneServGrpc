#pragma once
#include "util/test_framework.h"
namespace fuzz { TestCategory makeStringFuzz(const std::string& addr, int timeoutMs, bool fullMode); }
