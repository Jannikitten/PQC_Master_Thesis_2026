#ifndef PQC_MASTER_THESIS_2026_SERIALIZATION_H
#define PQC_MASTER_THESIS_2026_SERIALIZATION_H

// ═════════════════════════════════════════════════════════════════════════════
// Serialization.h — concept-based, non-virtual serialization system
//
// Replaces the old virtual StreamWriter / StreamReader hierarchy with
// concrete BufferWriter / BufferReader classes and free-function overloads
// constrained by C++20/23 concepts.
//
// §3.2  Result types   – deserialize<T>() returns std::expected<T, ParseError>
// §5.3  Serialization  – same wire format as the old system (binary-compatible)
// C++23               – concepts, std::expected, std::span
//
// Wire format:
//   Trivially-copyable types: raw memcpy of sizeof(T) bytes
//   Strings:  size_t (8 bytes on 64-bit) + char data  (matches old StreamWriter)
//   Vectors:  uint32_t count + elements
// ═════════════════════════════════════════════════════════════════════════════

#include "Types.h"

#include <concepts>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Safira {

// ─────────────────────────────────────────────────────────────────────────────
// BufferWriter — writes into a pre-allocated or growable byte buffer
//
// Two modes:
//   Fixed:   BufferWriter(MutableByteSpan)    — writes into a fixed region
//   Growing: BufferWriter(ByteBuffer&)        — resizes the vector on demand
// ─────────────────────────────────────────────────────────────────────────────
class BufferWriter {
public:
    explicit BufferWriter(MutableByteSpan target, size_t pos = 0) noexcept
        : m_Target(target.data()), m_Capacity(target.size()), m_Pos(pos) {}

    explicit BufferWriter(ByteBuffer& target, size_t pos = 0) noexcept
        : m_Owned(&target), m_Target(target.data()), m_Capacity(target.size()), m_Pos(pos) {}

    [[nodiscard]] bool WriteBytes(const void* data, size_t size) {
        if (m_Owned) {
            if (m_Pos + size > m_Capacity) {
                m_Owned->resize(m_Pos + size);
                m_Target   = m_Owned->data();
                m_Capacity = m_Owned->size();
            }
        } else {
            if (m_Pos + size > m_Capacity) return false;
        }
        std::memcpy(m_Target + m_Pos, data, size);
        m_Pos += size;
        return true;
    }

    [[nodiscard]] size_t GetPosition() const noexcept { return m_Pos; }
    void SetPosition(size_t p) noexcept { m_Pos = p; }

    /// View of bytes written so far.
    [[nodiscard]] ByteSpan Written() const noexcept {
        if (m_Owned) return ByteSpan(m_Owned->data(), m_Pos);
        return ByteSpan(m_Target, m_Pos);
    }

private:
    ByteBuffer* m_Owned    = nullptr;
    uint8_t*    m_Target   = nullptr;
    size_t      m_Capacity = 0;
    size_t      m_Pos      = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// BufferReader — reads from a ByteSpan, all reads return std::expected
// ─────────────────────────────────────────────────────────────────────────────
class BufferReader {
public:
    explicit BufferReader(ByteSpan source, size_t pos = 0) noexcept
        : m_Source(source), m_Pos(pos) {}

    [[nodiscard]] std::expected<void, ParseError> ReadBytes(void* dest, size_t size) {
        if (m_Pos + size > m_Source.size())
            return std::unexpected(ParseError::UnexpectedEnd);
        std::memcpy(dest, m_Source.data() + m_Pos, size);
        m_Pos += size;
        return {};
    }

    [[nodiscard]] size_t GetPosition() const noexcept { return m_Pos; }
    [[nodiscard]] size_t Remaining()   const noexcept { return m_Source.size() - m_Pos; }
    void SetPosition(size_t p) noexcept { m_Pos = p; }

private:
    ByteSpan m_Source;
    size_t   m_Pos = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations for concept definitions
// ─────────────────────────────────────────────────────────────────────────────

// Serialize: free function `Serialize(BufferWriter&, const T&) -> bool`
// found by ADL or in namespace Safira.
template <typename T>
concept Serializable = requires(BufferWriter& w, const T& val) {
    { Serialize(w, val) } -> std::same_as<bool>;
};

// Deserialize: free function `Deserialize<T>(BufferReader&) -> expected<T, ParseError>`
// (checked at call-site, not via concept, since template functions can't be
//  discovered by a concept expression without an explicit specialization.)

// ─────────────────────────────────────────────────────────────────────────────
// Built-in Serialize / Deserialize for trivially copyable types
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
    requires std::is_trivially_copyable_v<T> && (!std::is_same_v<T, std::string>)
[[nodiscard]] bool Serialize(BufferWriter& w, const T& val) {
    return w.WriteBytes(&val, sizeof(T));
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] std::expected<T, ParseError> Deserialize(BufferReader& r) {
    T val;
    auto result = r.ReadBytes(&val, sizeof(T));
    if (!result) return std::unexpected(result.error());
    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
// String serialization (wire format: size_t + char data)
//
// Matches the old StreamWriter::WriteString exactly.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline bool Serialize(BufferWriter& w, const std::string& s) {
    const size_t size = s.size();
    if (!w.WriteBytes(&size, sizeof(size_t))) return false;
    if (size > 0 && !w.WriteBytes(s.data(), size)) return false;
    return true;
}

[[nodiscard]] inline bool Serialize(BufferWriter& w, std::string_view s) {
    const size_t size = s.size();
    if (!w.WriteBytes(&size, sizeof(size_t))) return false;
    if (size > 0 && !w.WriteBytes(s.data(), size)) return false;
    return true;
}

template <typename T>
std::expected<T, ParseError> Deserialize(BufferReader& r);

template <>
[[nodiscard]] inline std::expected<std::string, ParseError>
Deserialize<std::string>(BufferReader& r) {
    auto sizeResult = Deserialize<size_t>(r);
    if (!sizeResult) return std::unexpected(sizeResult.error());

    const size_t size = *sizeResult;
    if (r.Remaining() < size)
        return std::unexpected(ParseError::UnexpectedEnd);

    std::string s(size, '\0');
    auto readResult = r.ReadBytes(s.data(), size);
    if (!readResult) return std::unexpected(readResult.error());
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Vector serialization (wire format: uint32_t count + elements)
//
// Matches the old StreamWriter::WriteArray exactly.
// ─────────────────────────────────────────────────────────────────────────────

template <Serializable T>
[[nodiscard]] bool Serialize(BufferWriter& w, const std::vector<T>& vec) {
    if (!Serialize(w, static_cast<uint32_t>(vec.size()))) return false;
    for (const auto& elem : vec)
        if (!Serialize(w, elem)) return false;
    return true;
}

template <typename T>
[[nodiscard]] std::expected<std::vector<T>, ParseError>
DeserializeVector(BufferReader& r) {
    auto count = Deserialize<uint32_t>(r);
    if (!count) return std::unexpected(count.error());

    std::vector<T> vec;
    vec.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
        auto elem = Deserialize<T>(r);
        if (!elem) return std::unexpected(elem.error());
        vec.push_back(std::move(*elem));
    }
    return vec;
}

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_SERIALIZATION_H
