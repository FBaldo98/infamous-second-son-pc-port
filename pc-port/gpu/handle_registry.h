// SPDX-License-Identifier: GPL-2.0-or-later
// Which image views and samplers exist, for fault reports (handle_registry.cpp).
#pragma once

#include <cstddef>
#include "common/types.h"

namespace iss::handles {

/// Wraps the create and destroy entry points of the default dispatcher (after its device init).
void Install();

/// False only for an image view known to be destroyed (null and untracked handles count as live).
bool IsLiveView(u64 handle);

/// "LIVE, created N ms ago", "DESTROYED N ms ago", "UNKNOWN" or "null"; text is the buffer used.
const char* DescribeView(u64 handle, char* text, std::size_t size);
const char* DescribeSampler(u64 handle, char* text, std::size_t size);

} // namespace iss::handles
