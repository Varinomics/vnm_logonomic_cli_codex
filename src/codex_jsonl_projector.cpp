#include "codex_jsonl_projector.h"


#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>
#include <utility>

namespace codex {
namespace addon = logonomic::cli_addon;
namespace {

constexpr std::size_t k_max_selected_string_bytes = 60 * 1024;
constexpr std::size_t k_max_selected_number_bytes = 512;
constexpr std::size_t k_max_selected_key_bytes = 256;
constexpr std::size_t k_max_json_depth = 1024;

enum class Container_kind
{
    OBJECT,
    ARRAY,
};

enum class Container_path
{
    NONE,
    ROOT,
    PAYLOAD,
    INFO,
    TOTAL_TOKEN_USAGE,
};

enum class Container_state
{
    KEY_OR_END,
    KEY,
    COLON,
    VALUE_OR_END,
    VALUE,
    COMMA_OR_END,
};

enum class Projection_target
{
    NONE,
    ROOT_TYPE,
    ROOT_TIMESTAMP,
    ROOT_PAYLOAD,
    PAYLOAD_ID,
    PAYLOAD_TIMESTAMP,
    PAYLOAD_FORKED_FROM_ID,
    PAYLOAD_CWD,
    PAYLOAD_MODEL,
    PAYLOAD_TYPE,
    PAYLOAD_TURN_ID,
    PAYLOAD_STARTED_AT,
    PAYLOAD_COMPLETED_AT,
    PAYLOAD_INFO,
    INFO_TOTAL_TOKEN_USAGE,
    USAGE_TOTAL_TOKENS,
    USAGE_INPUT_TOKENS,
    USAGE_OUTPUT_TOKENS,
    USAGE_CACHED_INPUT_TOKENS,
};

enum class Token_kind
{
    NONE,
    STRING,
    NUMBER,
    LITERAL,
};

enum class Number_state
{
    SIGN,
    ZERO,
    INTEGER,
    DECIMAL_POINT,
    FRACTION,
    EXPONENT_MARK,
    EXPONENT_SIGN,
    EXPONENT,
};

struct Container_frame final
{
    Container_kind kind = Container_kind::OBJECT;
    Container_path path = Container_path::NONE;
    Container_state state = Container_state::KEY_OR_END;
    Projection_target target = Projection_target::NONE;
};

bool json_whitespace(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r';
}

bool json_delimiter(char byte)
{
    return json_whitespace(byte) || byte == ',' || byte == ']' || byte == '}';
}

bool ascii_digit(char byte)
{
    return byte >= '0' && byte <= '9';
}

bool hex_digit(char byte)
{
    return ascii_digit(byte) ||
        (byte >= 'a' && byte <= 'f') ||
        (byte >= 'A' && byte <= 'F');
}

Projection_target target_for_key(
    Container_path path,
    const std::string& key)
{
    switch (path) {
    case Container_path::ROOT:
        if (key == "type") {
            return Projection_target::ROOT_TYPE;
        }
        if (key == "timestamp") {
            return Projection_target::ROOT_TIMESTAMP;
        }
        if (key == "payload") {
            return Projection_target::ROOT_PAYLOAD;
        }
        break;
    case Container_path::PAYLOAD:
        if (key == "id") {
            return Projection_target::PAYLOAD_ID;
        }
        if (key == "timestamp") {
            return Projection_target::PAYLOAD_TIMESTAMP;
        }
        if (key == "forked_from_id") {
            return Projection_target::PAYLOAD_FORKED_FROM_ID;
        }
        if (key == "cwd") {
            return Projection_target::PAYLOAD_CWD;
        }
        if (key == "model") {
            return Projection_target::PAYLOAD_MODEL;
        }
        if (key == "type") {
            return Projection_target::PAYLOAD_TYPE;
        }
        if (key == "turn_id") {
            return Projection_target::PAYLOAD_TURN_ID;
        }
        if (key == "started_at") {
            return Projection_target::PAYLOAD_STARTED_AT;
        }
        if (key == "completed_at") {
            return Projection_target::PAYLOAD_COMPLETED_AT;
        }
        if (key == "info") {
            return Projection_target::PAYLOAD_INFO;
        }
        break;
    case Container_path::INFO:
        if (key == "total_token_usage") {
            return Projection_target::INFO_TOTAL_TOKEN_USAGE;
        }
        break;
    case Container_path::TOTAL_TOKEN_USAGE:
        if (key == "total_tokens") {
            return Projection_target::USAGE_TOTAL_TOKENS;
        }
        if (key == "input_tokens") {
            return Projection_target::USAGE_INPUT_TOKENS;
        }
        if (key == "output_tokens") {
            return Projection_target::USAGE_OUTPUT_TOKENS;
        }
        if (key == "cached_input_tokens") {
            return Projection_target::USAGE_CACHED_INPUT_TOKENS;
        }
        break;
    case Container_path::NONE:
        break;
    }
    return Projection_target::NONE;
}

Container_path object_path_for_target(Projection_target target)
{
    switch (target) {
    case Projection_target::ROOT_PAYLOAD:
        return Container_path::PAYLOAD;
    case Projection_target::PAYLOAD_INFO:
        return Container_path::INFO;
    case Projection_target::INFO_TOTAL_TOKEN_USAGE:
        return Container_path::TOTAL_TOKEN_USAGE;
    default:
        return Container_path::NONE;
    }
}

std::string target_key(Projection_target target)
{
    switch (target) {
    case Projection_target::ROOT_TYPE:
        return "type";
    case Projection_target::ROOT_TIMESTAMP:
        return "timestamp";
    case Projection_target::ROOT_PAYLOAD:
        return "payload";
    case Projection_target::PAYLOAD_ID:
        return "id";
    case Projection_target::PAYLOAD_TIMESTAMP:
        return "timestamp";
    case Projection_target::PAYLOAD_FORKED_FROM_ID:
        return "forked_from_id";
    case Projection_target::PAYLOAD_CWD:
        return "cwd";
    case Projection_target::PAYLOAD_MODEL:
        return "model";
    case Projection_target::PAYLOAD_TYPE:
        return "type";
    case Projection_target::PAYLOAD_TURN_ID:
        return "turn_id";
    case Projection_target::PAYLOAD_STARTED_AT:
        return "started_at";
    case Projection_target::PAYLOAD_COMPLETED_AT:
        return "completed_at";
    case Projection_target::PAYLOAD_INFO:
        return "info";
    case Projection_target::INFO_TOTAL_TOKEN_USAGE:
        return "total_token_usage";
    case Projection_target::USAGE_TOTAL_TOKENS:
        return "total_tokens";
    case Projection_target::USAGE_INPUT_TOKENS:
        return "input_tokens";
    case Projection_target::USAGE_OUTPUT_TOKENS:
        return "output_tokens";
    case Projection_target::USAGE_CACHED_INPUT_TOKENS:
        return "cached_input_tokens";
    case Projection_target::NONE:
        break;
    }
    return {};
}

std::optional<addon::Json_value> decode_scalar(const std::string& bytes)
{
    addon::Json_parse_error error;
    return addon::parse_json(bytes, addon::Json_parse_limits{}, &error);
}

addon::Json_value object_member(const addon::Json_value& object, std::string_view key)
{
    const auto* member = object.find(key);
    return member && member->is_object() ? *member : addon::Json_value::object();
}
} // namespace

struct Codex_jsonl_projector::Impl final
{
    explicit Impl(Record_handler handler)
        : record_handler(std::move(handler))
    {
    }

    void reset_line()
    {
        frames.clear();
        projected = addon::Json_value::object();
        token_bytes.clear();
        literal.clear();
        token = Token_kind::NONE;
        number_state = Number_state::INTEGER;
        token_target = Projection_target::NONE;
        token_is_key = false;
        capture_token = false;
        string_escape = false;
        unicode_digits = 0;
        utf8_remaining = 0;
        utf8_code_point = 0;
        utf8_minimum = 0;
        invalid = false;
        root_complete = false;
        root_object = false;
        line_started = false;
        bom_state = 0;
    }

    void assign(Projection_target target, const addon::Json_value& value)
    {
        if (target == Projection_target::NONE) {
            return;
        }
        const std::string key = target_key(target);
        switch (target) {
        case Projection_target::ROOT_TYPE:
        case Projection_target::ROOT_TIMESTAMP:
        case Projection_target::ROOT_PAYLOAD:
            projected.set(key, value);
            return;
        case Projection_target::PAYLOAD_ID:
        case Projection_target::PAYLOAD_TIMESTAMP:
        case Projection_target::PAYLOAD_FORKED_FROM_ID:
        case Projection_target::PAYLOAD_CWD:
        case Projection_target::PAYLOAD_MODEL:
        case Projection_target::PAYLOAD_TYPE:
        case Projection_target::PAYLOAD_TURN_ID:
        case Projection_target::PAYLOAD_STARTED_AT:
        case Projection_target::PAYLOAD_COMPLETED_AT:
        case Projection_target::PAYLOAD_INFO: {
            addon::Json_value payload = object_member(projected, "payload");
            payload.set(key, value);
            projected.set("payload", payload);
            return;
        }
        case Projection_target::INFO_TOTAL_TOKEN_USAGE: {
            addon::Json_value payload = object_member(projected, "payload");
            addon::Json_value info = object_member(payload, "info");
            info.set(key, value);
            payload.set("info", info);
            projected.set("payload", payload);
            return;
        }
        case Projection_target::USAGE_TOTAL_TOKENS:
        case Projection_target::USAGE_INPUT_TOKENS:
        case Projection_target::USAGE_OUTPUT_TOKENS:
        case Projection_target::USAGE_CACHED_INPUT_TOKENS: {
            addon::Json_value payload = object_member(projected, "payload");
            addon::Json_value info = object_member(payload, "info");
            addon::Json_value usage = object_member(info, "total_token_usage");
            usage.set(key, value);
            info.set("total_token_usage", usage);
            payload.set("info", info);
            projected.set("payload", payload);
            return;
        }
        case Projection_target::NONE:
            return;
        }
    }

    Projection_target expected_target() const
    {
        if (frames.empty()) {
            return Projection_target::NONE;
        }
        return frames.back().target;
    }

    void value_complete()
    {
        if (frames.empty()) {
            root_complete = true;
            return;
        }
        Container_frame& frame = frames.back();
        frame.state = Container_state::COMMA_OR_END;
        frame.target = Projection_target::NONE;
    }

    void close_container(Container_kind kind)
    {
        if (frames.empty() || frames.back().kind != kind) {
            invalid = true;
            return;
        }
        const bool closed_root = frames.size() == 1;
        frames.pop_back();
        if (closed_root) {
            root_complete = true;
        }
        else {
            value_complete();
        }
    }

    void start_container(Container_kind kind, Projection_target target)
    {
        if (frames.size() >= k_max_json_depth) {
            invalid = true;
            return;
        }
        if (frames.empty()) {
            root_object = kind == Container_kind::OBJECT;
        }
        Container_path path = Container_path::NONE;
        if (frames.empty() && kind == Container_kind::OBJECT) {
            path = Container_path::ROOT;
        }
        else
        if (kind == Container_kind::OBJECT) {
            path = object_path_for_target(target);
        }
        if (target != Projection_target::NONE) {
            assign(
                target,
                kind == Container_kind::OBJECT
                    ? addon::Json_value::object()
                    : addon::Json_value::array());
        }
        frames.push_back(Container_frame{
            kind,
            path,
            kind == Container_kind::OBJECT
                ? Container_state::KEY_OR_END
                : Container_state::VALUE_OR_END,
            Projection_target::NONE,
        });
    }

    void start_string(bool key, Projection_target target)
    {
        token = Token_kind::STRING;
        token_is_key = key;
        token_target = target;
        capture_token = key
            ? !frames.empty() &&
                frames.back().path != Container_path::NONE
            : target != Projection_target::NONE;
        token_bytes.clear();
        if (capture_token) {
            token_bytes.push_back('"');
        }
        string_escape = false;
        unicode_digits = 0;
        utf8_remaining = 0;
    }

    void start_number(char byte, Projection_target target)
    {
        token = Token_kind::NUMBER;
        token_is_key = false;
        token_target = target;
        capture_token = target != Projection_target::NONE;
        token_bytes.clear();
        if (capture_token) {
            token_bytes.push_back(byte);
        }
        if (byte == '-') {
            number_state = Number_state::SIGN;
        }
        else
        if (byte == '0') {
            number_state = Number_state::ZERO;
        }
        else {
            number_state = Number_state::INTEGER;
        }
    }

    void start_literal(
        std::string expected,
        char byte,
        Projection_target target)
    {
        token = Token_kind::LITERAL;
        token_target = target;
        capture_token = target != Projection_target::NONE;
        literal = std::move(expected);
        literal_offset = 1;
        token_bytes.clear();
        if (capture_token) {
            token_bytes.push_back(byte);
        }
    }

    void start_value(char byte)
    {
        const Projection_target target = expected_target();
        if (byte == '{') {
            start_container(Container_kind::OBJECT, target);
            return;
        }
        if (byte == '[') {
            start_container(Container_kind::ARRAY, target);
            return;
        }
        if (byte == '"') {
            start_string(false, target);
            return;
        }
        if (byte == '-' || ascii_digit(byte)) {
            start_number(byte, target);
            return;
        }
        if (byte == 't') {
            start_literal("true", byte, target);
            return;
        }
        if (byte == 'f') {
            start_literal("false", byte, target);
            return;
        }
        if (byte == 'n') {
            start_literal("null", byte, target);
            return;
        }
        invalid = true;
    }

    bool append_captured(char byte, std::size_t maximum)
    {
        if (!capture_token) {
            return true;
        }
        if (token_bytes.size() >= maximum) {
            if (!token_is_key) {
                invalid = true;
            }
            capture_token = false;
            token_bytes.clear();
            return false;
        }
        token_bytes.push_back(byte);
        return true;
    }

    void consume_utf8(char byte)
    {
        const auto value = static_cast<unsigned char>(byte);
        if (utf8_remaining > 0) {
            if ((value & 0xc0U) != 0x80U) {
                invalid = true;
                utf8_remaining = 0;
                return;
            }
            utf8_code_point = (utf8_code_point << 6U) | (value & 0x3fU);
            --utf8_remaining;
            if (utf8_remaining == 0 &&
                (utf8_code_point < utf8_minimum ||
                 utf8_code_point > 0x10ffffU ||
                 (utf8_code_point >= 0xd800U &&
                  utf8_code_point <= 0xdfffU))) {
                invalid = true;
            }
            return;
        }
        if (value < 0x80U) {
            return;
        }
        if (value >= 0xc2U && value <= 0xdfU) {
            utf8_remaining = 1;
            utf8_code_point = value & 0x1fU;
            utf8_minimum = 0x80U;
            return;
        }
        if (value >= 0xe0U && value <= 0xefU) {
            utf8_remaining = 2;
            utf8_code_point = value & 0x0fU;
            utf8_minimum = 0x800U;
            return;
        }
        if (value >= 0xf0U && value <= 0xf4U) {
            utf8_remaining = 3;
            utf8_code_point = value & 0x07U;
            utf8_minimum = 0x10000U;
            return;
        }
        invalid = true;
    }

    void finish_string()
    {
        if (utf8_remaining != 0) {
            invalid = true;
        }
        if (token_is_key) {
            std::string key;
            if (capture_token) {
                const auto decoded = decode_scalar(token_bytes);
                if (!decoded || !decoded->is_string()) {
                    invalid = true;
                }
                else {
                    key = decoded->as_string();
                }
            }
            if (!frames.empty()) {
                Container_frame& frame = frames.back();
                frame.target = target_for_key(frame.path, key);
                frame.state = Container_state::COLON;
            }
        }
        else {
            if (capture_token) {
                const auto decoded = decode_scalar(token_bytes);
                if (!decoded) {
                    invalid = true;
                }
                else {
                    assign(token_target, *decoded);
                }
            }
            value_complete();
        }
        token = Token_kind::NONE;
        token_bytes.clear();
    }

    void consume_string(char byte)
    {
        const std::size_t maximum = token_is_key
            ? k_max_selected_key_bytes
            : k_max_selected_string_bytes;
        if (unicode_digits > 0) {
            if (!hex_digit(byte)) {
                invalid = true;
            }
            append_captured(byte, maximum);
            --unicode_digits;
            return;
        }
        if (string_escape) {
            string_escape = false;
            if (byte == 'u') {
                unicode_digits = 4;
            }
            else
            if (byte != '"' && byte != '\\' && byte != '/' &&
                     byte != 'b' && byte != 'f' && byte != 'n' &&
                     byte != 'r' && byte != 't') {
                invalid = true;
            }
            append_captured(byte, maximum);
            return;
        }
        if (byte == '"') {
            append_captured(byte, maximum);
            finish_string();
            return;
        }
        if (byte == '\\') {
            if (utf8_remaining != 0) {
                invalid = true;
                utf8_remaining = 0;
            }
            string_escape = true;
            append_captured(byte, maximum);
            return;
        }
        if (static_cast<unsigned char>(byte) < 0x20U) {
            invalid = true;
        }
        consume_utf8(byte);
        append_captured(byte, maximum);
    }

    bool number_can_finish() const
    {
        return number_state == Number_state::ZERO ||
            number_state == Number_state::INTEGER ||
            number_state == Number_state::FRACTION ||
            number_state == Number_state::EXPONENT;
    }

    void finish_number()
    {
        if (!number_can_finish()) {
            invalid = true;
        }
        if (capture_token) {
            const auto decoded = decode_scalar(token_bytes);
            if (!decoded || !decoded->is_number()) {
                invalid = true;
            }
            else {
                assign(token_target, *decoded);
            }
        }
        token = Token_kind::NONE;
        token_bytes.clear();
        value_complete();
    }

    bool consume_number(char byte)
    {
        if (json_delimiter(byte)) {
            finish_number();
            return false;
        }
        switch (number_state) {
        case Number_state::SIGN:
            if (byte == '0') {
                number_state = Number_state::ZERO;
            }
            else
            if (byte >= '1' && byte <= '9') {
                number_state = Number_state::INTEGER;
            }
            else {
                invalid = true;
            }
            break;
        case Number_state::ZERO:
            if (byte == '.') {
                number_state = Number_state::DECIMAL_POINT;
            }
            else
            if (byte == 'e' || byte == 'E') {
                number_state = Number_state::EXPONENT_MARK;
            }
            else {
                invalid = true;
            }
            break;
        case Number_state::INTEGER:
            if (ascii_digit(byte)) {
                break;
            }
            if (byte == '.') {
                number_state = Number_state::DECIMAL_POINT;
            }
            else
            if (byte == 'e' || byte == 'E') {
                number_state = Number_state::EXPONENT_MARK;
            }
            else {
                invalid = true;
            }
            break;
        case Number_state::DECIMAL_POINT:
            if (ascii_digit(byte)) {
                number_state = Number_state::FRACTION;
            }
            else {
                invalid = true;
            }
            break;
        case Number_state::FRACTION:
            if (ascii_digit(byte)) {
                break;
            }
            if (byte == 'e' || byte == 'E') {
                number_state = Number_state::EXPONENT_MARK;
            }
            else {
                invalid = true;
            }
            break;
        case Number_state::EXPONENT_MARK:
            if (byte == '+' || byte == '-') {
                number_state = Number_state::EXPONENT_SIGN;
            }
            else
            if (ascii_digit(byte)) {
                number_state = Number_state::EXPONENT;
            }
            else {
                invalid = true;
            }
            break;
        case Number_state::EXPONENT_SIGN:
            if (ascii_digit(byte)) {
                number_state = Number_state::EXPONENT;
            }
            else {
                invalid = true;
            }
            break;
        case Number_state::EXPONENT:
            if (!ascii_digit(byte)) {
                invalid = true;
            }
            break;
        }
        append_captured(byte, k_max_selected_number_bytes);
        return true;
    }

    bool consume_literal(char byte)
    {
        if (literal_offset < literal.size()) {
            if (byte != literal.at(literal_offset)) {
                invalid = true;
            }
            ++literal_offset;
            if (capture_token) {
                token_bytes.push_back(byte);
            }
            return true;
        }
        if (!json_delimiter(byte)) {
            invalid = true;
            return true;
        }
        if (capture_token) {
            const auto decoded = decode_scalar(token_bytes);
            if (!decoded) {
                invalid = true;
            }
            else {
                assign(token_target, *decoded);
            }
        }
        token = Token_kind::NONE;
        token_bytes.clear();
        value_complete();
        return false;
    }

    void consume_structure(char byte)
    {
        if (root_complete) {
            if (!json_whitespace(byte)) {
                invalid = true;
            }
            return;
        }
        if (frames.empty()) {
            if (json_whitespace(byte)) {
                return;
            }
            start_value(byte);
            return;
        }

        Container_frame& frame = frames.back();
        switch (frame.state) {
        case Container_state::KEY_OR_END:
            if (json_whitespace(byte)) {
                return;
            }
            if (byte == '}') {
                close_container(Container_kind::OBJECT);
                return;
            }
            if (byte == '"') {
                start_string(true, Projection_target::NONE);
                return;
            }
            invalid = true;
            return;
        case Container_state::KEY:
            if (json_whitespace(byte)) {
                return;
            }
            if (byte == '"') {
                start_string(true, Projection_target::NONE);
                return;
            }
            invalid = true;
            return;
        case Container_state::COLON:
            if (json_whitespace(byte)) {
                return;
            }
            if (byte == ':') {
                frame.state = Container_state::VALUE;
                return;
            }
            invalid = true;
            return;
        case Container_state::VALUE_OR_END:
            if (json_whitespace(byte)) {
                return;
            }
            if (byte == ']') {
                close_container(Container_kind::ARRAY);
                return;
            }
            start_value(byte);
            return;
        case Container_state::VALUE:
            if (json_whitespace(byte)) {
                return;
            }
            start_value(byte);
            return;
        case Container_state::COMMA_OR_END:
            if (json_whitespace(byte)) {
                return;
            }
            if (byte == ',') {
                frame.state = frame.kind == Container_kind::OBJECT
                    ? Container_state::KEY
                    : Container_state::VALUE;
                return;
            }
            if (frame.kind == Container_kind::OBJECT && byte == '}') {
                close_container(Container_kind::OBJECT);
                return;
            }
            if (frame.kind == Container_kind::ARRAY && byte == ']') {
                close_container(Container_kind::ARRAY);
                return;
            }
            invalid = true;
            return;
        }
    }

    bool consume_byte(char byte)
    {
        if (!line_started) {
            if (byte == '\0') {
                return true;
            }
            line_started = true;
            if (static_cast<unsigned char>(byte) == 0xefU) {
                bom_state = 1;
                return true;
            }
        }
        if (bom_state != 0) {
            if (bom_state == 1 &&
                static_cast<unsigned char>(byte) == 0xbbU) {
                bom_state = 2;
                return true;
            }
            if (bom_state == 2 &&
                static_cast<unsigned char>(byte) == 0xbfU) {
                bom_state = 0;
                return true;
            }
            invalid = true;
            bom_state = 0;
        }
        if (invalid) {
            return true;
        }
        bool consumed = false;
        while (!consumed) {
            switch (token) {
            case Token_kind::STRING:
                consume_string(byte);
                consumed = true;
                break;
            case Token_kind::NUMBER:
                consumed = consume_number(byte);
                break;
            case Token_kind::LITERAL:
                consumed = consume_literal(byte);
                break;
            case Token_kind::NONE:
                consume_structure(byte);
                consumed = true;
                break;
            }
        }
        return true;
    }

    bool finish_line(std::string* out_error)
    {
        if (token == Token_kind::NUMBER) {
            finish_number();
        }
        else
        if (token == Token_kind::LITERAL &&
                 literal_offset == literal.size()) {
            if (capture_token) {
                const auto decoded = decode_scalar(token_bytes);
                if (!decoded) {
                    invalid = true;
                }
                else {
                    assign(token_target, *decoded);
                }
            }
            token = Token_kind::NONE;
            value_complete();
        }
        if (invalid || token != Token_kind::NONE || !root_complete ||
            !root_object || !frames.empty()) {
            if (out_error) {
                *out_error = "Codex JSONL contains a malformed record at line " + std::to_string(current_line) + ".";
            }
            return false;
        }
        if (!record_handler(projected)) {
            return false;
        }
        ++current_line;
        reset_line();
        return true;
    }

    Record_handler record_handler;
    std::vector<Container_frame> frames;
    addon::Json_value projected = addon::Json_value::object();
    std::string token_bytes;
    std::string literal;
    Token_kind token = Token_kind::NONE;
    Number_state number_state = Number_state::INTEGER;
    Projection_target token_target = Projection_target::NONE;
    std::size_t literal_offset = 0;
    int current_line = 1;
    int unicode_digits = 0;
    int utf8_remaining = 0;
    int bom_state = 0;
    uint32_t utf8_code_point = 0;
    uint32_t utf8_minimum = 0;
    bool token_is_key = false;
    bool capture_token = false;
    bool string_escape = false;
    bool invalid = false;
    bool root_complete = false;
    bool root_object = false;
    bool line_started = false;
};

Codex_jsonl_projector::Codex_jsonl_projector(Record_handler record_handler)
    : m_impl(std::make_unique<Impl>(std::move(record_handler)))
{
}

Codex_jsonl_projector::~Codex_jsonl_projector() = default;

bool Codex_jsonl_projector::feed(std::string_view bytes, std::string* out_error)
{
    for (const char byte : bytes) {
        if (byte == '\n') {
            if (!m_impl->finish_line(out_error)) {
                return false;
            }
            continue;
        }
        m_impl->consume_byte(byte);
    }
    return true;
}

bool Codex_jsonl_projector::finish_file()
{
    m_impl->reset_line();
    return true;
}

int Codex_jsonl_projector::line_number() const
{
    return m_impl->current_line;
}

std::size_t Codex_jsonl_projector::retained_bytes_for_tests() const
{
    return m_impl->token_bytes.capacity() +
        static_cast<std::size_t>(m_impl->frames.capacity()) *
            static_cast<std::size_t>(sizeof(Container_frame));
}

} // namespace codex
