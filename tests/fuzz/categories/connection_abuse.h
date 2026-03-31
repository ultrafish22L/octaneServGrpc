#pragma once
#include "util/test_framework.h"
namespace fuzz { TestCategory makeConnectionAbuse(const std::string& addr, int timeoutMs); }
