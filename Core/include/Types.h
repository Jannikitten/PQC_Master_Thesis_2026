#ifndef PQC_MASTER_THESIS_2026_TYPES_H
#define PQC_MASTER_THESIS_2026_TYPES_H

// ═════════════════════════════════════════════════════════════════════════════
// Types.h — project-wide vocabulary types for the functional C++23 codebase
//
// §3.2  Result types   – ParseError enum for fallible deserialization
// §5.5  Value types    – ByteBuffer / ByteSpan replace raw void* Buffer
// C++23               – std::span, std::vector, std::expected
// ═════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// Owning buffer — replaces the old Buffer class for all owning use cases.
// ─────────────────────────────────────────────────────────────────────────────
using ByteBuffer = std::vector<uint8_t>;

// ─────────────────────────────────────────────────────────────────────────────
// Non-owning views — replaces Buffer for read-only and mutable borrowed access.
// ─────────────────────────────────────────────────────────────────────────────
using ByteSpan        = std::span<const uint8_t>;
using MutableByteSpan = std::span<uint8_t>;

// ─────────────────────────────────────────────────────────────────────────────
// §3.2 — Parse error type for fallible deserialization
//
// Every deserialize<T>() returns std::expected<T, ParseError>.
// ─────────────────────────────────────────────────────────────────────────────
enum class ParseError : uint8_t {
    UnexpectedEnd,
    InvalidPacketType,
    InvalidData,
    StringTooLong,
};

[[nodiscard]] constexpr std::string_view Describe(ParseError e) noexcept {
    switch (e) {
        case ParseError::UnexpectedEnd:     return "unexpected end of buffer";
        case ParseError::InvalidPacketType: return "unrecognised packet type tag";
        case ParseError::InvalidData:       return "field value out of range";
        case ParseError::StringTooLong:     return "string exceeds max length";
    }
    return "unknown parse error";
}

// ─────────────────────────────────────────────────────────────────────────────
// Overloaded — visitor helper for std::visit
//
// Previously duplicated in Server.cpp and Client.cpp.  Centralised here so
// all variant dispatch sites use the same definition.
//
//   std::visit(Overloaded {
//       [](const A& a) { ... },
//       [](const B& b) { ... },
//   }, myVariant);
// ─────────────────────────────────────────────────────────────────────────────
template <typename... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_TYPES_H
