#include "codex_hook_metadata.h"
#include "codex_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/model_state.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void native_start_model()
{
    // Oracle: Codex's SessionStartRequest.model is the agent's live
    // TurnContext model; model-state@1 confines it to its accepted transition.
    const auto native = *addon::parse_json(R"({"hook_event_name":"SessionStart",
        "session_id":"native-session","source":"startup","model":"gateway/model-current"})", {}, nullptr);
    addon::Classify_event_request request;
    request.event = {"SessionStart", codex::hook_metadata(native)};
    request.transition_nonce = std::string(32, 'a');
    auto result = codex::classify_hook(request);
    require(result && result->model_updates.empty(), "Unrequested model capability must remain absent");
    request.model_state_version = addon::k_model_state_version;
    result = codex::classify_hook(request);
    require(result && result->classification == addon::Classification::TRANSITION &&
        result->observations.size() == 1 && result->model_updates.size() == 1 && !result->model_failure,
        "Native start must carry lifecycle and model evidence together");
    const auto& update = result->model_updates.front();
    require(update.model == "gateway/model-current" && update.provider.empty() && update.agent_id.empty() &&
        update.provider_session_id == "native-session" && update.transition_nonce == request.transition_nonce &&
        update.sequence == 0 && update.status == addon::Model_status::CONFIRMED,
        "Native literal model must not infer a backend or another actor");
    const auto decoded = addon::decode_classify_event_result(addon::encode_classify_event_result(*result), nullptr);
    require(decoded && decoded->model_updates == result->model_updates && !decoded->model_failure,
        "Optional model must round-trip through the actual carrier");

    auto oversized_object = Json::object();
    oversized_object.set("model", Json(std::string(48 * 1024, 'm')));
    const std::vector<Json> malformed_models{
        Json(""), Json(std::string(513, 'm')), Json(std::string(48 * 1024, 'm')),
        std::move(oversized_object), Json("model\ncontrol"), Json(17), Json(),
    };
    for (const auto& model : malformed_models) {
        auto malformed = native;
        malformed.set("model", model);
        request.event.body = codex::hook_metadata(malformed);
        require(addon::serialize_json(request.event.body).size() < addon::limits::k_max_provider_event_bytes,
            "Malformed optional model must not exceed the helper transport body limit");
        result = codex::classify_hook(request);
        require(result && result->classification == addon::Classification::TRANSITION &&
            result->observations.size() == 1 && result->model_updates.empty() && result->model_failure,
            "Malformed model must fail only the optional capability");
    }
    request.event.body = codex::hook_metadata(native);
    request.event.body.set("source", Json("compact"));
    result = codex::classify_hook(request);
    require(result && result->model_updates.empty() && result->observations.empty(),
        "Compaction must not relabel or reactivate the root");

    request.event.body = codex::hook_metadata(native);
    request.event.body.set("agent_id", Json("child"));
    result = codex::classify_hook(request);
    require(result && result->model_updates.empty(), "A child's SessionStart must not label its parent");
    request.event.body = codex::hook_metadata(native);
    request.model_failure = addon::Contract_failure{"unsupported_version", "fixture unsupported request"};
    result = codex::classify_hook(request);
    require(result && result->model_updates.empty() && result->model_failure && result->observations.size() == 1,
        "An unsupported model envelope must preserve lifecycle classification");
}

} // namespace

int main()
{
    try {
        native_start_model();
        return 0;
    }
    catch (const std::exception& failure) {
        std::cerr << "Codex model contract: " << failure.what() << '\n';
        return 1;
    }
}
