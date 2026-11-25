#pragma once
#include <cstdint>
#include <memory>

#include "gpu/backend.h"

namespace gpu {

std::unique_ptr<Backend> CreateBackend();

}  // namespace gpu
