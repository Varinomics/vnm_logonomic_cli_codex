#include "codex_pet_inspection.h"

#include <logonomic_cli_addon/account_pet.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/limits.h>

#include <exception>
#include <iostream>
#include <string>

int main()
{
    namespace addon = logonomic::cli_addon;

    try {
        std::string input;
        char byte = 0;
        while (std::cin.get(byte) && byte != '\n') {
            if (input.size() >= addon::limits::k_max_frame_bytes) {
                return 1;
            }
            input.push_back(byte);
        }
        const auto payload = addon::parse_json(input, {}, nullptr);
        const auto request = payload ? addon::decode_account_pet_request(*payload, nullptr) : std::nullopt;
        if (!request || !request->account_context.profile_path) {
            return 1;
        }
        const auto result = codex::inspect_account_pet(*request);
        std::cout << addon::serialize_json(addon::encode_account_pet_inspect_result(result)) << '\n';
        std::cout.flush();
        return std::cout ? 0 : 1;
    }
    catch (const std::exception&) {
        return 1;
    }
}
