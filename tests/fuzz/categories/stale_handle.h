#pragma once
#include "util/test_framework.h"
namespace fuzz { TestCategory makeStaleHandle(const std::string& addr, int timeoutMs); }
