#ifndef PQC_MASTER_THESIS_2026_SAFIRAASSERT_H
#define PQC_MASTER_THESIS_2026_SAFIRAASSERT_H

#ifndef PQC_MASTER_THESIS_2026_ASSERT_H
#define PQC_MASTER_THESIS_2026_ASSERT_H

#include "Log.h"

#ifdef WL_PLATFORM_WINDOWS
	#define WL_DEBUG_BREAK __debugbreak()
#elif defined(WL_COMPILER_CLANG)
	#define WL_DEBUG_BREAK __builtin_debugtrap()
#else
	#define WL_DEBUG_BREAK
#endif

#ifdef WL_DEBUG
	#define WL_ENABLE_ASSERTS
#endif

#define WL_ENABLE_VERIFY

#ifdef WL_ENABLE_ASSERTS
	#ifdef WL_COMPILER_CLANG
		#define WL_CORE_ASSERT_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Core, "Assertion Failed", ##__VA_ARGS__)
		#define WL_ASSERT_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Client, "Assertion Failed", ##__VA_ARGS__)
	#else
		#define WL_CORE_ASSERT_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Core, "Assertion Failed" __VA_OPT__(,) __VA_ARGS__)
		#define WL_ASSERT_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Client, "Assertion Failed" __VA_OPT__(,) __VA_ARGS__)
	#endif

	#define WL_CORE_ASSERT(condition, ...) { if(!(condition)) { WL_CORE_ASSERT_MESSAGE_INTERNAL(__VA_ARGS__); WL_DEBUG_BREAK; } }
	#define WL_ASSERT(condition, ...) { if(!(condition)) { WL_ASSERT_MESSAGE_INTERNAL(__VA_ARGS__); WL_DEBUG_BREAK; } }
#else
	#define WL_CORE_ASSERT(condition, ...)
	#define WL_ASSERT(condition, ...)
#endif

#ifdef WL_ENABLE_VERIFY
	#ifdef WL_COMPILER_CLANG
		#define WL_CORE_VERIFY_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Core, "Verify Failed", ##__VA_ARGS__)
		#define WL_VERIFY_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Client, "Verify Failed", ##__VA_ARGS__)
	#else
		#define WL_CORE_VERIFY_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Core, "Verify Failed" __VA_OPT__(,) __VA_ARGS__)
		#define WL_VERIFY_MESSAGE_INTERNAL(...)  ::Safira::Log::PrintAssertMessage(::Safira::Log::Type::Client, "Verify Failed" __VA_OPT__(,) __VA_ARGS__)
	#endif

	#define WL_CORE_VERIFY(condition, ...) { if(!(condition)) { WL_CORE_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); WL_DEBUG_BREAK; } }
	#define WL_VERIFY(condition, ...) { if(!(condition)) { WL_VERIFY_MESSAGE_INTERNAL(__VA_ARGS__); WL_DEBUG_BREAK; } }
#else
	#define WL_CORE_VERIFY(condition, ...)
	#define WL_VERIFY(condition, ...)
#endif

#endif //PQC_MASTER_THESIS_2026_ASSERT_H

#endif //PQC_MASTER_THESIS_2026_SAFIRAASSERT_H