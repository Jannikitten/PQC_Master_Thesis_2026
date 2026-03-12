#ifndef SAFIRA_APPLICATION_CLIENT_CLIENTWIRING_H
#define SAFIRA_APPLICATION_CLIENT_CLIENTWIRING_H

// ═════════════════════════════════════════════════════════════════════════════
// ClientWiring.h — Factory for the fully wired client store
//
// Assembles the Store with the complete middleware stack:
//   1. Deserialize middleware  — DataReceived → typed actions
//   2. Effect middleware       — runs effects (network + logging)
//   3. Logging middleware      — debug action logging
//   4. ApplyReducer            — pure state update
//
// Usage:
//   auto store = Safira::CreateClientStore(initialState, effectHandler);
//   store->Dispatch(action);
// ═════════════════════════════════════════════════════════════════════════════

#include "Store.h"
#include "Middleware.h"
#include "ClientState.h"
#include "ClientAction.h"
#include "ClientReducer.h"
#include "ClientEffects.h"
#include "ClientDeserialize.h"

#include <memory>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// CreateClientStore — builds the client store with all middleware installed
//
// Middleware execution order (for each Dispatch):
//   Deserialize → Effect → Logging → ApplyReducer
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline auto CreateClientStore(
    ClientState initialState,
    Middleware::ClientEffectHandler effectHandler)
{
    auto store = std::make_unique<Store<ClientState, ClientActionVariant>>(
        std::move(initialState), ClientReducerFn);

    // 1. Deserialize — outermost, converts raw DataReceived to typed actions
    store->AddMiddleware(Middleware::MakeClientDeserializeMiddleware());

    // 2. Effect — computes and executes effects
    store->AddMiddleware(
        Middleware::MakeClientEffectMiddleware(std::move(effectHandler)));

    // 3. Logging — innermost middleware, logs action dispatch
    store->AddMiddleware(
        Middleware::MakeLoggingMiddleware<ClientState, ClientActionVariant>("Client"));

    return store;
}

} // namespace Safira

#endif // SAFIRA_APPLICATION_CLIENT_CLIENTWIRING_H
