#ifndef SAFIRA_APPLICATION_SERVER_SERVERWIRING_H
#define SAFIRA_APPLICATION_SERVER_SERVERWIRING_H

// ═════════════════════════════════════════════════════════════════════════════
// ServerWiring.h — Factory for the fully wired server store
//
// Assembles the Store with the complete middleware stack:
//   1. Deserialize middleware  — DataReceived → typed actions
//   2. Effect middleware       — runs effects (protocol + infrastructure)
//   3. Logging middleware      — debug action logging
//   4. ApplyReducer            — pure state update
//
// Usage:
//   auto store = Safira::CreateServerStore(initialState, effectHandler);
//   store->Dispatch(action);
// ═════════════════════════════════════════════════════════════════════════════

#include "Store.h"
#include "Middleware.h"
#include "ServerState.h"
#include "ServerAction.h"
#include "ServerReducer.h"
#include "ServerEffects.h"
#include "ServerDeserialize.h"

#include <memory>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// CreateServerStore — builds the store with all middleware installed
//
// Middleware execution order (for each Dispatch):
//   Deserialize → Effect → Logging → ApplyReducer
//
// AddMiddleware appends to the list; the chain is built back-to-front
// (last middleware wraps the reducer, first middleware is outermost).
// So we add in the order: [Deserialize, Effect, Logging].
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline auto CreateServerStore(
    ServerState initialState,
    Middleware::ServerEffectHandler effectHandler)
{
    auto store = std::make_unique<Store<ServerState, ServerAction>>(
        std::move(initialState), ServerReducerFn);

    // 1. Deserialize — outermost, converts raw DataReceived to typed actions
    store->AddMiddleware(Middleware::MakeServerDeserializeMiddleware());

    // 2. Effect — computes and executes effects (protocol + infra)
    store->AddMiddleware(
        Middleware::MakeServerEffectMiddleware(std::move(effectHandler)));

    // 3. Logging — innermost middleware, logs action dispatch
    store->AddMiddleware(
        Middleware::MakeLoggingMiddleware<ServerState, ServerAction>("Server"));

    return store;
}

} // namespace Safira

#endif // SAFIRA_APPLICATION_SERVER_SERVERWIRING_H
