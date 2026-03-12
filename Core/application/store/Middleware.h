#ifndef SAFIRA_APPLICATION_STORE_MIDDLEWARE_H
#define SAFIRA_APPLICATION_STORE_MIDDLEWARE_H

// ═════════════════════════════════════════════════════════════════════════════
// Middleware.h — Common middleware utilities
//
// Middleware sits between Dispatch and the reducer.  It can:
//   - Log actions (debugging)
//   - Execute side effects described by the reducer
//   - Transform or filter actions
// ═════════════════════════════════════════════════════════════════════════════

#include "Store.h"

#include <functional>
#include <spdlog/spdlog.h>

namespace Safira::Middleware {

// ─────────────────────────────────────────────────────────────────────────────
// LoggingMiddleware — logs every action dispatch (debug builds)
// ─────────────────────────────────────────────────────────────────────────────
template <typename State, typename Action>
auto MakeLoggingMiddleware(std::string_view storeName) {
    return [name = std::string(storeName)](
        const Store<State, Action>&,
        const Action& action,
        typename Store<State, Action>::DispatchFn next)
    {
        spdlog::debug("[{}] dispatch action (variant index {})",
                      name, action.index());
        next(action);
    };
}

} // namespace Safira::Middleware

#endif // SAFIRA_APPLICATION_STORE_MIDDLEWARE_H
