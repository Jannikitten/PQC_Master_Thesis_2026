#ifndef SAFIRA_DOMAIN_PORTS_PERSISTENCEPORT_H
#define SAFIRA_DOMAIN_PORTS_PERSISTENCEPORT_H

// ═════════════════════════════════════════════════════════════════════════════
// Defines what the application layer needs for saving/loading chat history
// without prescribing the storage format (YAML, SQLite, etc.).
// ═════════════════════════════════════════════════════════════════════════════

#include "Message.h"

#include <concepts>
#include <expected>
#include <string>
#include <vector>

namespace Safira {

    // ─────────────────────────────────────────────────────────────────────────────
    // MessageStore — save and load chat message history
    // ─────────────────────────────────────────────────────────────────────────────
    template <typename T>
    concept MessageStore = requires(T& store,
                                    const std::vector<ChatMessage>& msgs) {
        { store.Save(msgs) }        -> std::same_as<bool>;
        { store.Load() }            -> std::same_as<std::expected<std::vector<ChatMessage>, std::string>>;
    };

} // namespace Safira

#endif // SAFIRA_DOMAIN_PORTS_PERSISTENCEPORT_H
