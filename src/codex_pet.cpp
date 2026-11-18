#include "codex_pet.h"
#include "codex_accounts.h"

#include <logonomic_cli_addon/account_pet.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codex {
namespace {

namespace addon = logonomic::cli_addon;

// A stalled profile read must settle its helper before the common request deadline.
constexpr int k_pet_inspection_budget_ms = 3000;

std::optional<addon::Account_pet_inspect_result> inspect_with_helper(
    const addon::Account_pet_request& request, addon::Invocation_context& context)
{
    const auto environment = addon::current_process_environment();
    const auto package = addon::path_from_utf8(addon::environment_value(environment, addon::k_env_package_root));
    if (!package.is_absolute()) {
        return std::nullopt;
    }
#ifdef _WIN32
    const auto helper = package / "helpers" / "codex_pet_helper.exe";
#else
    const auto helper = package / "helpers" / "codex_pet_helper";
#endif
    addon::Probe_exchange exchange;
    exchange.initial_input = addon::serialize_json(addon::encode_account_pet_request(
        request.account_context, request.normalization_directory, request.home_directory)) + '\n';
    std::optional<addon::Account_pet_inspect_result> result;
    exchange.on_output = [
            &exchange,
            &result,
            &request
        ](
            std::string_view bytes
        ) -> std::optional<std::string>
        {
            const auto end = bytes.find('\n');
            if (end == std::string_view::npos) {
                return std::string{};
            }
            const auto payload = addon::parse_json(bytes.substr(0, end), {}, nullptr);
            if (!payload || addon::account_context_acknowledgment_failure(request.account_context, *payload)) {
                return std::nullopt;
            }
            result = addon::decode_account_pet_inspect_result(*payload, nullptr);
            if (!result) {
                return std::nullopt;
            }
            exchange.complete = true;
            return std::string{};
        };
    const auto observed = addon::probe(
        {addon::path_to_utf8(helper)}, environment, addon::path_to_utf8(package),
        k_pet_inspection_budget_ms, context, &exchange);
    return observed && exchange.complete ? std::move(result) : std::nullopt;
}

} // namespace

void handle_account_pet(addon::Invocation_context& context)
{
    if (context.request().operation != "inspect") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_account_pet_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& account = request->account_context;
    const auto selected = granted_profile(account, account.profile_path.value_or(""));
    if (!selected) {
        context.fail(addon::Result_status::UNAVAILABLE, "account_context_mismatch");
        return;
    }
    auto resolved = *request;
    resolved.account_context.profile_path = selected->profile_path;
    auto inspected = inspect_with_helper(resolved, context);
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    if (!inspected) {
        addon::Account_pet_inspect_result unavailable;
        unavailable.account_context = account;
        unavailable.unavailable_reason = "pet_inspection_unavailable";
        inspected = std::move(unavailable);
    }
    inspected->account_context = account;
    context.succeed(addon::encode_account_pet_inspect_result(*inspected));
}

} // namespace codex
