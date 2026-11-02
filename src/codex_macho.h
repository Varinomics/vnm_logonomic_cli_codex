#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>

namespace codex::detail {

enum class Macho_architecture
{
    X86_64,
    ARM64,
};

inline std::uint32_t macho_word(const unsigned char* bytes, bool little_endian)
{
    if (little_endian) {
        return bytes[0] | (std::uint32_t(bytes[1]) << 8) |
            (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    }
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
        (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

inline std::optional<Macho_architecture> macho_architecture(std::uint32_t cpu)
{
    switch (cpu) {
        case 0x01000007: return Macho_architecture::X86_64;
        case 0x0100000c: return Macho_architecture::ARM64;
        default:         return std::nullopt;
    }
}

inline std::optional<Macho_architecture> thin_macho_target(
    const std::array<unsigned char, 32>& header, std::uint64_t size)
{
    // Node and Codex's Darwin targets are little-endian 64-bit executables.
    if (macho_word(header.data(), true) != 0xfeedfacf ||
        macho_word(header.data() + 12, true) != 2 ||
        macho_word(header.data() + 20, true) > size - header.size())
    {
        return std::nullopt;
    }
    return macho_architecture(macho_word(header.data() + 4, true));
}

// Keep identification portable so the format checks run on every host. The
// actual --version probe still establishes that the selected chain executes.
inline std::optional<Macho_architecture> macho_target(
    std::istream& input, Macho_architecture preferred)
{
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return std::nullopt;
    }
    const auto file_size = (std::uint64_t)(std::streamoff)end;
    const auto read = [
            &input,
            file_size
        ](
            std::uint64_t offset,
            unsigned char* bytes,
            std::size_t size)
        {
            if (offset > file_size || size > file_size - offset) {
                return false;
            }
            input.seekg((std::streamoff)offset);
            input.read(reinterpret_cast<char*>(bytes), (std::streamsize)size);
            return input.good();
        };
    std::array<unsigned char, 32> header{};
    if (!read(0, header.data(), header.size())) {
        return std::nullopt;
    }
    const auto magic = macho_word(header.data(), false);
    if (magic == 0xcffaedfe) {
        return thin_macho_target(header, file_size);
    }
    const bool wide = magic == 0xcafebabf;
    if (magic != 0xcafebabe && !wide) {
        return std::nullopt;
    }
    const auto count      = macho_word(header.data() + 4, false);
    const auto entry_size = wide ? 32u : 20u;
    const auto table_end  = 8u + std::uint64_t(count) * entry_size;
    // Native launch recognition must not scan an unbounded architecture table.
    if (count == 0 || count > 64 || table_end > file_size) {
        return std::nullopt;
    }
    std::optional<Macho_architecture> selected;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::array<unsigned char, 32> entry{};
        if (!read(8u + std::uint64_t(index) * entry_size, entry.data(), entry_size)) {
            return std::nullopt;
        }
        const auto architecture = macho_architecture(macho_word(entry.data(), false));
        const std::uint64_t offset = wide
            ? (std::uint64_t(macho_word(entry.data() + 8, false)) << 32) |
                macho_word(entry.data() + 12, false)
            : macho_word(entry.data() + 8, false);
        const std::uint64_t size = wide
            ? (std::uint64_t(macho_word(entry.data() + 16, false)) << 32) |
                macho_word(entry.data() + 20, false)
            : macho_word(entry.data() + 12, false);
        if (offset < table_end || offset > file_size || size == 0 || size > file_size - offset) {
            return std::nullopt;
        }
        if (!architecture) {
            continue;
        }
        if (size < header.size() || !read(offset, header.data(), header.size()) ||
            thin_macho_target(header, size) != architecture)
        {
            return std::nullopt;
        }
        if (!selected || architecture == preferred) {
            selected = architecture;
        }
    }
    return selected;
}

} // namespace codex::detail
