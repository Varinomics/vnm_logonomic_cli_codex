#pragma once

#include <logonomic_cli_addon/json.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace codex {

// Projects only usage-bearing metadata while validating every JSON token.
// Ignored prompts/tool output never accumulate in memory, including on a line
// much larger than a transport frame. An incomplete final line is not a record.
class Codex_jsonl_projector final
{
public:
    using Record_handler = std::function<bool(const logonomic::cli_addon::Json_value&)>;

    explicit Codex_jsonl_projector(Record_handler handler);
    ~Codex_jsonl_projector();
    Codex_jsonl_projector(const Codex_jsonl_projector&) = delete;
    Codex_jsonl_projector& operator=(const Codex_jsonl_projector&) = delete;
    bool feed(std::string_view bytes, std::string* error);
    bool finish_file();
    int line_number() const;
    std::size_t retained_bytes_for_tests() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace codex
