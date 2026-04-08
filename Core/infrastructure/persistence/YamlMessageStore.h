#ifndef SAFIRA_INFRASTRUCTURE_PERSISTENCE_YAMLMESSAGESTORE_H
#define SAFIRA_INFRASTRUCTURE_PERSISTENCE_YAMLMESSAGESTORE_H

// ═════════════════════════════════════════════════════════════════════════════
// YamlMessageStore.h — YAML-backed message persistence
//
// Satisfies the domain::MessageStore concept.  Extracted from ServerLayer's
// SaveMessageHistoryToFile / LoadMessageHistoryFromFile methods.
// ═════════════════════════════════════════════════════════════════════════════

#include "Message.h"
#include "PersistencePort.h"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace Safira {

    class YamlMessageStore {
    public:
        explicit YamlMessageStore(std::filesystem::path filepath,
                                  std::size_t maxMessages = 5000)
            : m_FilePath(std::move(filepath))
            , m_MaxMessages(maxMessages) {}

        [[nodiscard]] bool Save(const std::vector<ChatMessage>& messages);
        [[nodiscard]] std::expected<std::vector<ChatMessage>, std::string> Load();

    private:
        std::filesystem::path m_FilePath;
        std::size_t                m_MaxMessages;
    };

    // Verify concept satisfaction
    static_assert(MessageStore<YamlMessageStore>);

} // namespace Safira

#endif // SAFIRA_INFRASTRUCTURE_PERSISTENCE_YAMLMESSAGESTORE_H
