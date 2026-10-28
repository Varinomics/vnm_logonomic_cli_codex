#include <logonomic_cli_addon/json.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv)
{
    namespace addon = logonomic::cli_addon;
    const char* capture = std::getenv("CODEX_TEST_CAPTURE");
    if (argc < 2 || !capture) {
        return 2;
    }
    const std::string operation = argv[1];
    auto record = addon::Json_value::object();
    if (operation == "provider-event" && argc == 4 && std::string(argv[2]) == "acknowledge") {
        record.set("operation", addon::Json_value("acknowledge"));
        record.set("idempotency_key", addon::Json_value(argv[3]));
        std::ofstream(capture, std::ios::app) << addon::serialize_json(record) << '\n';
        return 0;
    }
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), {}};
    const auto request = addon::parse_json(bytes, {}, nullptr);
    if (!request || !request->is_object()) {
        return 2;
    }
    record.set("operation", addon::Json_value(operation));
    record.set("request", *request);
    std::ofstream(capture, std::ios::app) << addon::serialize_json(record) << '\n';
    if (operation == "request-title-instruction") {
        std::cout << R"({"accepted":true,"instruction_required":false,"instruction":""})" << '\n';
        return 0;
    }
    if (operation != "provider-event" || argc != 4 || std::string(argv[2]) != "deliver") {
        return 2;
    }
    std::cout << R"({"accepted":true,"epoch":2})" << '\n';
    return 0;
}
