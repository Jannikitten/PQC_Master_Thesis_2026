#ifndef SAFIRA_DOMAIN_RULES_RATELIMIT_H
#define SAFIRA_DOMAIN_RULES_RATELIMIT_H

// ═════════════════════════════════════════════════════════════════════════════
// The RateLimitState is a value type.  CheckRateLimit is a pure function
// that takes a state + current time and returns a new state + decision.
// No mutexes, no I/O — caller manages storage and threading.
// ═════════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <chrono>
#include <vector>

namespace Safira::Rules {

    // ─────────────────────────────────────────────────────────────────────────────
    // Policy constants
    // ─────────────────────────────────────────────────────────────────────────────
    static constexpr int   kRateLimitMessages   = 10;     // messages per window
    static constexpr float kRateLimitWindowSec  = 5.0f;   // sliding window
    static constexpr int   kFloodKickThreshold  = 3;      // violations before auto-kick

    // ─────────────────────────────────────────────────────────────────────────────
    // RateLimitState — immutable value type (create new copies, don't mutate)
    // ─────────────────────────────────────────────────────────────────────────────
    struct RateLimitState {
        std::vector<std::chrono::steady_clock::time_point> Timestamps;
        int Violations = 0;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // RateLimitResult — outcome of a rate-limit check
    // ─────────────────────────────────────────────────────────────────────────────
    enum class RateLimitDecision : uint8_t {
        Allowed,       // Message passes
        Throttled,     // Message dropped (rate exceeded)
        Kick,          // Client should be disconnected (flood)
    };

    struct RateLimitResult {
        RateLimitState  NewState;
        RateLimitDecision Decision;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // CheckRateLimit — pure function
    //
    // Given the current state and the current time, produces a new state
    // and a decision.  The caller applies the decision (kick, drop, allow).
    // ─────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] inline RateLimitResult
    CheckRateLimit(const RateLimitState& state,
                   std::chrono::steady_clock::time_point now) {

        const auto window = std::chrono::duration<float>(kRateLimitWindowSec);

        // Purge timestamps outside the window
        RateLimitState next;
        next.Violations = state.Violations;

        for (const auto& tp : state.Timestamps) {
            if ((now - tp) <= window)
                next.Timestamps.push_back(tp);
        }

        // Check if over the limit
        if (static_cast<int>(next.Timestamps.size()) >= kRateLimitMessages) {
            next.Violations++;

            if (next.Violations >= kFloodKickThreshold)
                return { std::move(next), RateLimitDecision::Kick };

            return { std::move(next), RateLimitDecision::Throttled };
        }

        // Allowed — record the timestamp
        next.Timestamps.push_back(now);
        return { std::move(next), RateLimitDecision::Allowed };
    }

} // namespace Safira::Rules

#endif // SAFIRA_DOMAIN_RULES_RATELIMIT_H
