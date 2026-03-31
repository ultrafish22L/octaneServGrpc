#pragma once
#include "util/test_framework.h"
namespace fuzz { TestCategory makeStateMachine(const std::string& addr, int timeoutMs); }
