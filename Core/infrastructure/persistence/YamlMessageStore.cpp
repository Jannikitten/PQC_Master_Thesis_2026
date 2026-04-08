#include "YamlMessageStore.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <spdlog/spdlog.h>

namespace Safira {

    bool YamlMessageStore::Save(const std::vector<ChatMessage>& messages) {
        YAML::Emitter out;
        out << YAML::BeginMap
            << YAML::Key << "MessageHistory"
            << YAML::Value << YAML::BeginSeq;

        // Only save the last m_MaxMessages
        const std::size_t start = (messages.size() > m_MaxMessages)
            ? (messages.size() - m_MaxMessages)
            : 0;

        for (std::size_t i = start; i < messages.size(); ++i) {
            out << YAML::BeginMap
                << YAML::Key << "User"    << YAML::Value << messages[i].Username
                << YAML::Key << "Message" << YAML::Value << messages[i].Message
                << YAML::EndMap;
        }

        out << YAML::EndSeq << YAML::EndMap;

        std::ofstream fout(m_FilePath);
        if (!fout) return false;
        fout << out.c_str();
        return fout.good();
    }

    std::expected<std::vector<ChatMessage>, std::string>
    YamlMessageStore::Load() {
        if (!std::filesystem::exists(m_FilePath))
            return std::vector<ChatMessage>{};

        YAML::Node data;
        try {
            data = YAML::LoadFile(m_FilePath.string());
        } catch (const YAML::ParserException& e) {
            return std::unexpected(
                std::string("YAML parse error: ") + e.what());
        }

        auto root = data["MessageHistory"];
        if (!root)
            return std::vector<ChatMessage>{};

        std::vector<ChatMessage> messages;
        messages.reserve(root.size());

        for (const auto& node : root) {
            messages.emplace_back(
                node["User"].as<std::string>(),
                node["Message"].as<std::string>());
        }

        // Trim to max
        if (messages.size() > m_MaxMessages) {
            messages.erase(
                messages.begin(),
                messages.begin() + static_cast<std::ptrdiff_t>(
                    messages.size() - m_MaxMessages));
        }

        return messages;
    }

} // namespace Safira
