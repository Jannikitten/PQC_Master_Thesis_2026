#ifndef PQC_MASTER_THESIS_2026_LOG_H
#define PQC_MASTER_THESIS_2026_LOG_H


#include "LogCustomFormatters.h"

#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h"

#include <map>

#define WL_ASSERT_MESSAGE_BOX (!WL_DIST && WL_PLATFORM_WINDOWS)

#if WL_ASSERT_MESSAGE_BOX
	#ifdef WL_PLATFORM_WINDOWS
		#include <Windows.h>
	#endif
#endif

namespace Safira {

	class Log
	{
	public:
		enum class Type : uint8_t
		{
			Core = 0, Client = 1
		};
		enum class Level : uint8_t
		{
			Trace = 0, Info, Warn, Error, Fatal
		};
		struct TagDetails
		{
			bool Enabled = true;
			Level LevelFilter = Level::Trace;
		};

	public:
		static void Init();
		static void Shutdown();

		inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }

		static bool HasTag(const std::string& tag) { return s_EnabledTags.find(tag) != s_EnabledTags.end(); }
		static std::map<std::string, TagDetails>& EnabledTags() { return s_EnabledTags; }

		template<typename... Args>
		static void PrintMessage(Log::Type type, Log::Level level, std::string_view tag, Args&&... args);

		template<typename... Args>
		static void PrintAssertMessage(Log::Type type, std::string_view prefix, Args&&... args);

	public:
		// Enum utils
		static const char* LevelToString(Level level)
		{
			switch (level)
			{
				case Level::Trace: return "Trace";
				case Level::Info:  return "Info";
				case Level::Warn:  return "Warn";
				case Level::Error: return "Error";
				case Level::Fatal: return "Fatal";
			}
			return "";
		}
		static Level LevelFromString(std::string_view string)
		{
			if (string == "Trace") return Level::Trace;
			if (string == "Info")  return Level::Info;
			if (string == "Warn")  return Level::Warn;
			if (string == "Error") return Level::Error;
			if (string == "Fatal") return Level::Fatal;

			return Level::Trace;
		}

	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;

		inline static std::map<std::string, TagDetails> s_EnabledTags;
	};

}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tagged logs (prefer these!)                                                                                      //
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Core logging
#define WL_CORE_TRACE_TAG(tag, ...) ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Trace, tag, __VA_ARGS__)
#define WL_CORE_INFO_TAG(tag, ...)  ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Info, tag, __VA_ARGS__)
#define WL_CORE_WARN_TAG(tag, ...)  ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Warn, tag, __VA_ARGS__)
#define WL_CORE_ERROR_TAG(tag, ...) ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Error, tag, __VA_ARGS__)
#define WL_CORE_FATAL_TAG(tag, ...) ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Fatal, tag, __VA_ARGS__)

// Client logging
#define WL_TRACE_TAG(tag, ...) ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Trace, tag, __VA_ARGS__)
#define WL_INFO_TAG(tag, ...)  ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Info, tag, __VA_ARGS__)
#define WL_WARN_TAG(tag, ...)  ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Warn, tag, __VA_ARGS__)
#define WL_ERROR_TAG(tag, ...) ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Error, tag, __VA_ARGS__)
#define WL_FATAL_TAG(tag, ...) ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Fatal, tag, __VA_ARGS__)

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Core Logging
#define WL_CORE_TRACE(...)  ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Trace, "", __VA_ARGS__)
#define WL_CORE_INFO(...)   ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Info, "", __VA_ARGS__)
#define WL_CORE_WARN(...)   ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Warn, "", __VA_ARGS__)
#define WL_CORE_ERROR(...)  ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Error, "", __VA_ARGS__)
#define WL_CORE_FATAL(...)  ::Safira::Log::PrintMessage(::Safira::Log::Type::Core, ::Safira::Log::Level::Fatal, "", __VA_ARGS__)

// Client Logging
#define WL_TRACE(...)   ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Trace, "", __VA_ARGS__)
#define WL_INFO(...)    ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Info, "", __VA_ARGS__)
#define WL_WARN(...)    ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Warn, "", __VA_ARGS__)
#define WL_ERROR(...)   ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Error, "", __VA_ARGS__)
#define WL_FATAL(...)   ::Safira::Log::PrintMessage(::Safira::Log::Type::Client, ::Safira::Log::Level::Fatal, "", __VA_ARGS__)

namespace Safira {

	template<typename... Args>
	void Log::PrintMessage(Log::Type type, Log::Level level, std::string_view tag, Args&&... args)
	{
		auto detail = s_EnabledTags[std::string(tag)];
		if (detail.Enabled && detail.LevelFilter <= level)
		{
			auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();
			std::string logString = tag.empty() ? "{0}{1}" : "[{0}] {1}";
			switch (level)
			{
			case Level::Trace:
				//logger->trace(fmt::vformat(logString, fmt::make_format_args(tag, fmt::make_format_args(args...))));
				logger->trace(fmt::format(logString, tag, fmt::vformat(fmt::make_format_args(args...))));
				break;
			case Level::Info:
				logger->info(fmt::format(logString, tag, fmt::vformat(fmt::make_format_args(args...))));
				break;
			case Level::Warn:
				logger->warn(fmt::format(logString, tag, fmt::vformat(fmt::make_format_args(args...))));
				break;
			case Level::Error:
				logger->error(fmt::format(logString, tag, fmt::vformat(fmt::make_format_args(args...))));
				break;
			case Level::Fatal:
				logger->critical(fmt::format(logString, tag, fmt::vformat(fmt::make_format_args(args...))));
				break;
			}
		}
	}


	template<typename... Args>
	void Log::PrintAssertMessage(Log::Type type, std::string_view prefix, Args&&... args)
	{
		auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();
		logger->error("{0}: {1}", prefix, fmt::vformat(fmt::make_format_args(args...)));

#if WL_ASSERT_MESSAGE_BOX
		std::string message = fmt::vformat(fmt::make_format_args(args...));
		MessageBoxA(nullptr, message.c_str(), "Safira Assert", MB_OK | MB_ICONERROR);
#endif
	}

	template<>
	inline void Log::PrintAssertMessage(Log::Type type, std::string_view prefix)
	{
		auto logger = (type == Type::Core) ? GetCoreLogger() : GetClientLogger();
		logger->error("{0}", prefix);
#if WL_ASSERT_MESSAGE_BOX
		MessageBoxA(nullptr, "No message :(", "Safira Assert", MB_OK | MB_ICONERROR);
#endif
	}
}


#endif //PQC_MASTER_THESIS_2026_LOG_H