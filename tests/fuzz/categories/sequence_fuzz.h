#pragma once
#include "util/test_framework.h"
namespace fuzz { TestCategory makeSequenceFuzz(const std::string& addr, int timeoutMs); }
