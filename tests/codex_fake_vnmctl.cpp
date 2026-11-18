#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/json_utilities.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

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
    // The native model fixture runs the packaged classifier before replying
    // to the still-live synchronous hook.
    if (const auto* directory = std::getenv("CODEX_TEST_CLASSIFIER_DIRECTORY")) {
        const auto key = addon::json::text(*request, "idempotency_key");
        if (key.size() != 32 || key.find_first_not_of("0123456789abcdef") != std::string::npos) {
            return 2;
        }
        const auto base = std::filesystem::path(directory) / key;
        const auto input = base.string() + ".pending";
        std::ofstream(input) << bytes;
        std::filesystem::rename(input, base.string() + ".request");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            std::ifstream response(base.string() + ".response");
            if (response) {
                std::cout << response.rdbuf();
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < end);
        return 1;
    }
    std::cout << R"({"accepted":true,"epoch":2})" << '\n';
    return 0;
}
