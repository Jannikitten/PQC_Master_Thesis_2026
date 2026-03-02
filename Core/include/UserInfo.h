#ifndef PQC_MASTER_THESIS_2026_USERINFO_H
#define PQC_MASTER_THESIS_2026_USERINFO_H

#include "StreamReader.h"
#include "StreamWriter.h"

namespace Safira {
    // ─────────────────────────────────────────────────────────────────────────────
    // Icons — 8 predefined coloured circles drawn via ImDrawList.
    // The index is stored in UserInfo and transmitted with every client list update.
    // ─────────────────────────────────────────────────────────────────────────────
    namespace Icons {
        static constexpr int kCount = 8;

        // ABGR colours (ImGui IM_COL32 byte order)
        static constexpr uint32_t kColors[kCount] = {
            0xFF4444EE, // Red
            0xFF44CC44, // Green
            0xFFEE8844, // Orange
            0xFF44BBEE, // Sky blue
            0xFFEEEE44, // Yellow
            0xFFCC44CC, // Purple
            0xFF44EECC, // Teal
            0xFFEEEEEE, // White
        };

        static constexpr const char* kLabels[kCount] = {
            "Red", "Green", "Orange", "Blue",
            "Yellow", "Purple", "Teal", "White",
        };
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // UserInfo
    // ─────────────────────────────────────────────────────────────────────────────
    struct UserInfo {
        uint32_t    Color     = 0xFFFFFFFF;
        std::string Username;
        uint8_t     IconIndex = 0;          // index into Icons::kColors[]

        static void Serialize(Safira::StreamWriter* serializer, const UserInfo& instance)
        {
            serializer->WriteRaw(instance.Color);
            serializer->WriteString(instance.Username);
            serializer->WriteRaw(instance.IconIndex);
        }

        static void Deserialize(Safira::StreamReader* deserializer, UserInfo& instance)
        {
            deserializer->ReadRaw(instance.Color);
            deserializer->ReadString(instance.Username);
            deserializer->ReadRaw(instance.IconIndex);
        }
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // ChatMessage
    // ─────────────────────────────────────────────────────────────────────────────
    struct ChatMessage
    {
        std::string Username;
        std::string Message;

        ChatMessage() = default;
        ChatMessage(const std::string& username, const std::string& message)
            : Username(username), Message(message) {}

        static void Serialize(Safira::StreamWriter* serializer, const ChatMessage& instance)
        {
            serializer->WriteString(instance.Username);
            serializer->WriteString(instance.Message);
        }

        static void Deserialize(Safira::StreamReader* deserializer, ChatMessage& instance)
        {
            deserializer->ReadString(instance.Username);
            deserializer->ReadString(instance.Message);
        }
    };

    const int MaxMessageLength = 4096;
    bool IsValidMessage(std::string& message);
}

#endif //PQC_MASTER_THESIS_2026_USERINFO_H