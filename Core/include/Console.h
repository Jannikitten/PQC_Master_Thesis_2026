#ifndef PQC_MASTER_THESIS_2026_CONSOLE_H
#define PQC_MASTER_THESIS_2026_CONSOLE_H

#include <vector>
#include <string>
#include <string_view>
#include <functional>
#include <iostream>

#include "spdlog/spdlog.h"

class Console {
public:
	using MessageSendCallback = std::function<void(std::string_view)>;

	Console(std::string_view title = "Safira Console");
	~Console();

	void ClearLog();

	template<typename... Args>
	void AddMessage(std::string_view format, Args&&... args) {
		const std::string messageString = fmt::vformat(format, fmt::make_format_args(args...));
		std::cout << messageString << std::endl;
		m_MessageHistory.push_back(messageString);
	}

	template<typename... Args>
	void AddItalicMessage(std::string_view format, Args&&... args) {
		std::string messageString = fmt::vformat(format, fmt::make_format_args(args...));
		MessageInfo info = messageString;
		info.Italic = true;
		m_MessageHistory.push_back(info);
		std::cout << messageString << std::endl;
	}

	template<typename... Args>
	void AddTaggedMessage(std::string_view tag, std::string_view format, Args&&... args) {
		std::string messageString = fmt::vformat(format, fmt::make_format_args(args...));
		m_MessageHistory.push_back(MessageInfo(std::string(tag), messageString));
		std::cout << '[' << tag << "] " << messageString << std::endl;
	}

	template<typename... Args>
	void AddMessageWithColor(uint32_t color, std::string_view format, Args&&... args) {
		std::string messageString = fmt::vformat(format, fmt::make_format_args(args...));
		m_MessageHistory.push_back(MessageInfo(messageString, color));
		std::cout << messageString << std::endl;
	}

	template<typename... Args>
	void AddItalicMessageWithColor(uint32_t color, std::string_view format, Args&&... args) {
		std::string messageString = fmt::vformat(format, fmt::make_format_args(args...));
		MessageInfo info(messageString, color);
		info.Italic = true;
		m_MessageHistory.push_back(info);
		std::cout << messageString << std::endl;
	}

	template<typename... Args>
	void AddTaggedMessageWithColor(uint32_t color, std::string_view tag, std::string_view format, Args&&... args) {
		std::string messageString = fmt::vformat(format, fmt::make_format_args(args...));
		m_MessageHistory.push_back(MessageInfo(std::string(tag), messageString, color));
		std::cout << '[' << tag << "] " << messageString << std::endl;
	}

	void OnUIRender() {}

	void SetMessageSendCallback(const MessageSendCallback& callback);
private:
	void InputThreadFunc();

	struct MessageInfo {
		std::string Tag;
		std::string Message;
		bool Italic = false;
		uint32_t Color = 0xffffffff;

		MessageInfo(const std::string& message, uint32_t color = 0xffffffff)
			: Message(message), Color(color) {}

		MessageInfo(const std::string& tag, const std::string& message, uint32_t color = 0xffffffff)
			: Tag(tag), Message(message), Color(color) {}
	};

	std::string m_Title;
	std::vector<MessageInfo> m_MessageHistory;

	std::thread m_InputThread;
	bool m_InputThreadRunning = false;

	MessageSendCallback m_MessageSendCallback;
};

#endif //PQC_MASTER_THESIS_2026_CONSOLE_H