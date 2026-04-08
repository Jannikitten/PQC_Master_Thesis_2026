#ifndef SAFIRA_APPLICATION_STORE_STORE_H
#define SAFIRA_APPLICATION_STORE_STORE_H

// ═════════════════════════════════════════════════════════════════════════════
// Strict FP principles:
//   - State is immutable from the outside (GetState returns const&)
//   - Reducer is a pure function: (State, Action) -> State
//   - Side effects are isolated in middleware
//   - Subscribers are notified after each state transition
//
// Thread safety: Dispatch() acquires a lock, so it can be called from
// any thread (e.g. network callbacks).  Subscribers run under the lock
// so they should be lightweight — queue work rather than doing heavy I/O.
// ═════════════════════════════════════════════════════════════════════════════

#include <functional>
#include <mutex>
#include <vector>

namespace Safira {

    template <typename State, typename Action>
    class Store {
    public:
        // Pure reducer: (old state, action) -> new state
        using ReducerFn    = State(*)(const State&, const Action&);

        // Subscriber: called after every state transition with new state
        using SubscriberFn = std::function<void(const State&)>;

        // Dispatch function type (passed to middleware)
        using DispatchFn   = std::function<void(const Action&)>;

        // Middleware: intercepts actions before/after the reducer
        // Receives store ref (for GetState), the action, and a "next" dispatch
        using MiddlewareFn = std::function<void(const Store&, const Action&, DispatchFn next)>;

        // ─────────────────────────────────────────────────────────────────────
        // Construction
        // ─────────────────────────────────────────────────────────────────────
        Store(State initial, ReducerFn reducer)
            : m_State(std::move(initial))
            , m_Reducer(reducer)
        {}

        // ─────────────────────────────────────────────────────────────────────
        // Dispatch — the only way to mutate state
        //
        // Actions flow through the middleware chain before reaching the
        // reducer.  Each middleware can:
        //   - Forward to next (possibly after logging / analytics)
        //   - Swallow the action (don't call next)
        //   - Dispatch additional actions (side effects)
        // ─────────────────────────────────────────────────────────────────────
        void Dispatch(const Action& action) {
            std::lock_guard lock(m_Mutex);

            if (m_Middleware.empty()) {
                ApplyReducer(action);
            } else {
                // Build the middleware chain from back to front
                DispatchFn chain = [this](const Action& a) { ApplyReducer(a); };

                for (auto it = m_Middleware.rbegin(); it != m_Middleware.rend(); ++it) {
                    const auto& mw = *it;
                    chain = [this, &mw, next = std::move(chain)](const Action& a) {
                        mw(*this, a, next);
                    };
                }

                chain(action);
            }
        }

        // ─────────────────────────────────────────────────────────────────────
        // Read-only state access
        // ─────────────────────────────────────────────────────────────────────
        [[nodiscard]] const State& GetState() const noexcept {
            return m_State;
        }

        // ─────────────────────────────────────────────────────────────────────
        // Subscribe — called after every state transition
        // ─────────────────────────────────────────────────────────────────────
        void Subscribe(SubscriberFn fn) {
            std::lock_guard lock(m_Mutex);
            m_Subscribers.push_back(std::move(fn));
        }

        // ─────────────────────────────────────────────────────────────────────
        // AddMiddleware — must be called before first Dispatch
        // ─────────────────────────────────────────────────────────────────────
        void AddMiddleware(MiddlewareFn mw) {
            std::lock_guard lock(m_Mutex);
            m_Middleware.push_back(std::move(mw));
        }

    private:
        void ApplyReducer(const Action& action) {
            m_State = m_Reducer(m_State, action);
            for (const auto& sub : m_Subscribers) {
                sub(m_State);
            }
        }

        State                       m_State;
        ReducerFn                   m_Reducer;
        std::vector<MiddlewareFn>   m_Middleware;
        std::vector<SubscriberFn>   m_Subscribers;
        mutable std::mutex          m_Mutex;
    };

} // namespace Safira

#endif // SAFIRA_APPLICATION_STORE_STORE_H
