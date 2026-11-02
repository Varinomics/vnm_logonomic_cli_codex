#include "codex_macho.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using codex::detail::Macho_architecture;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void word(std::string& bytes, std::size_t offset, std::uint32_t value, bool little_endian)
{
    for (unsigned index = 0; index < 4; ++index) {
        bytes.at(offset + index) = (char)(value >> (8 * (little_endian ? index : 3 - index)));
    }
}

void wide_word(std::string& bytes, std::size_t offset, std::uint64_t value)
{
    word(bytes, offset,     (std::uint32_t)(value >> 32), false);
    word(bytes, offset + 4, (std::uint32_t)value,         false);
}

std::string thin(std::uint32_t cpu, bool little_endian = true)
{
    std::string bytes(32, '\0');
    word(bytes,  0, 0xfeedfacf, little_endian);
    word(bytes,  4, cpu,        little_endian);
    word(bytes, 12, 2,          little_endian);
    return bytes;
}

std::string universal(bool wide, bool arm_first)
{
    const unsigned entry_size = wide ? 32 : 20;
    std::string bytes(160, '\0');
    word(bytes, 0, wide ? 0xcafebabf : 0xcafebabe, false);
    word(bytes, 4, 2, false);
    for (unsigned index = 0; index < 2; ++index) {
        const std::uint32_t cpu = (index == 0) == arm_first ? 0x0100000c : 0x01000007;
        const unsigned entry   = 8 + index * entry_size;
        const unsigned offset  = 80 + index * 48;
        word(bytes, entry, cpu, false);
        if (wide) {
            wide_word(bytes, entry + 8,  offset);
            wide_word(bytes, entry + 16, 32);
        }
        else {
            word(bytes, entry + 8,  offset, false);
            word(bytes, entry + 12, 32,     false);
        }
        bytes.replace(offset, 32, thin(cpu));
    }
    return bytes;
}

std::optional<Macho_architecture> target(
    const std::string& bytes, Macho_architecture preferred = Macho_architecture::ARM64)
{
    std::istringstream input(bytes);
    return codex::detail::macho_target(input, preferred);
}

void check_headers()
{
    // Oracle: Apple's mach_header_64, fat_arch/fat_arch_64 and CPU_TYPE
    // definitions; these fixtures identify images, they do not execute them.
    require(target(thin(0x0100000c)) == Macho_architecture::ARM64,
        "Thin ARM64 Mach-O must select the Darwin ARM64 payload");
    require(target(thin(0x01000007)) == Macho_architecture::X86_64,
        "Thin x86_64 Node must select its own architecture even on an ARM64 host");
    require(target(thin(0x0100000c), Macho_architecture::X86_64) == Macho_architecture::ARM64,
        "A thin image's architecture comes from its header");
    for (std::size_t length = 0; length < 32; ++length) {
        require(!target(thin(0x0100000c).substr(0, length)), "Truncated Mach-O header accepted");
    }
    for (const std::uint32_t cpu : {7u, 12u, 0x01000012u}) {
        require(!target(thin(cpu)), "Unsupported Mach-O architecture accepted");
    }
    auto invalid = thin(0x0100000c);
    word(invalid, 0, 0xfeedface, true);
    require(!target(invalid), "32-bit Mach-O magic accepted as a 64-bit image");
    invalid = thin(0x0100000c);
    word(invalid, 12, 6, true);
    require(!target(invalid), "A dynamic library accepted as a native command");
    invalid = thin(0x0100000c);
    word(invalid, 20, 8, true);
    require(!target(invalid), "Truncated load-command region accepted");
    require(!target(thin(0x0100000c, false)), "Big-endian ARM64 image accepted");
}

void check_universal()
{
    for (const bool wide : {false, true}) {
        for (const bool arm_first : {false, true}) {
            const auto bytes = universal(wide, arm_first);
            require(target(bytes) == Macho_architecture::ARM64,
                "Universal Node must select ARM64 independently of table order");
            require(target(bytes, Macho_architecture::X86_64) == Macho_architecture::X86_64,
                "Universal payload must preserve the architecture chosen for Node");
        }
        auto invalid = universal(wide, true);
        word(invalid, 4, 0, false);
        require(!target(invalid), "An empty universal table accepted");
        word(invalid, 4, std::numeric_limits<std::uint32_t>::max(), false);
        require(!target(invalid), "An unbounded universal table accepted");
        invalid = universal(wide, true).substr(0, wide ? 71 : 47);
        require(!target(invalid), "A truncated universal table accepted");
        invalid = universal(wide, true);
        word(invalid, 84, 0x01000007, true);
        require(!target(invalid), "Universal table and slice architecture disagreement accepted");
        invalid = universal(wide, true);
        if (wide) {
            wide_word(invalid, 16, std::numeric_limits<std::uint64_t>::max());
        }
        else {
            word(invalid, 16, std::numeric_limits<std::uint32_t>::max(), false);
        }
        require(!target(invalid), "Out-of-file universal slice offset accepted");
        invalid = universal(wide, true);
        if (wide) {
            wide_word(invalid, 24, std::numeric_limits<std::uint64_t>::max());
        }
        else {
            word(invalid, 20, std::numeric_limits<std::uint32_t>::max(), false);
        }
        require(!target(invalid), "Out-of-file universal slice size accepted");
        invalid = universal(wide, true);
        if (wide) {
            wide_word(invalid, 16, 8);
        }
        else {
            word(invalid, 16, 8, false);
        }
        require(!target(invalid), "A universal slice overlapping its table accepted");
    }
    auto x64_only = universal(false, false);
    word(x64_only, 4, 1, false);
    require(target(x64_only) == Macho_architecture::X86_64,
        "A universal image with only x86_64 keeps the thin x86_64 launch path");
}

} // namespace

int main()
{
    try {
        check_headers();
        check_universal();
        std::cout << "Mach-O header and universal-slice checks passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
