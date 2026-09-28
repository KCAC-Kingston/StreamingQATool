#pragma once

namespace RequestPolicy {
constexpr int idleMs = 5 * 60 * 1000;
constexpr int prestartMs = 15000;
constexpr int streamingMs = 30000;
constexpr int endingMs = 15000;
constexpr int prestartTimeoutMs = 20 * 60 * 1000;
constexpr int commandRetries = 3; // In addition to the initial attempt.
constexpr int retryDelayMs = 1000;

inline int pollInterval(bool ending, bool countdown, bool live, bool preparing)
{
	if (ending && !countdown)
		return endingMs;
	if (live || countdown)
		return streamingMs;
	return preparing ? prestartMs : idleMs;
}
} // namespace RequestPolicy
