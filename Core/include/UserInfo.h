#ifndef PQC_MASTER_THESIS_2026_USERINFO_H
#define PQC_MASTER_THESIS_2026_USERINFO_H


#include "StreamReader.h"
#include "StreamWriter.h"

struct UserInfo {
    uint32_t Color;
    std::string Username;

    static void Serialize(Safira::StreamWriter* serializer, const UserInfo& instance)
    {
        serializer->WriteRaw(instance.Color);
        serializer->WriteString(instance.Username);
    }

    static void Deserialize(Safira::StreamReader* deserializer, UserInfo& instance)
    {
        deserializer->ReadRaw(instance.Color);
        deserializer->ReadString(instance.Username);
    }
};

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


#endif //PQC_MASTER_THESIS_2026_USERINFO_H