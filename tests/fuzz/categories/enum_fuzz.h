#pragma once
#include "util/test_framework.h"
namespace fuzz { TestCategory makeEnumFuzz(const std::string& addr, int timeoutMs); }
