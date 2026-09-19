#include "codex_startup_observation.h"

#include <iostream>

int main()
{
    const std::string start = R"({"key":"event.name","value":{"stringValue":"codex.conversation_starts"}})";
    const auto record = [&](const std::string& id) {
        return "{\"attributes\":[" + start + ","
            "{\"key\":\"conversation.id\",\"value\":{\"stringValue\":\"" + id + "\"}}]}";
    };
    const auto batch = [](const std::string& records) {
        return "{\"resourceLogs\":[{\"scopeLogs\":[{\"logRecords\":[" + records + "]}]}]}";
    };
    const auto observed = codex::conversation_ids(batch(record("exact-a") + "," +
        record("exact-a") + "," + record("other-b")));
    if (!observed || *observed != std::vector<std::string>{"exact-a", "other-b"}) {
        std::cerr << "Startup identity order/deduplication lost a conflicting conversation\n";
        return 1;
    }
    const auto irrelevant = codex::conversation_ids("{\"SessionStart\":\"exact-a\"}");
    if (!irrelevant || !irrelevant->empty() || codex::conversation_ids("{broken")) {
        std::cerr << "Only native OTLP conversation-start evidence may confirm startup\n";
        return 1;
    }
    const auto ambiguous = codex::conversation_ids(batch("{\"attributes\":[" + start + "," + start +
        ",{\"key\":\"conversation.id\",\"value\":{\"stringValue\":\"exact-a\"}}]}"));
    if (!ambiguous || !ambiguous->empty()) {
        std::cerr << "Duplicate event authority was accepted\n";
        return 1;
    }
    std::string records;
    for (int i = 0; i < 65; ++i) {
        if (!records.empty()) records += ',';
        records += record("id-" + std::to_string(i));
    }
    if (codex::conversation_ids(batch(records))) {
        std::cerr << "Overflow silently discarded startup evidence\n";
        return 1;
    }
    return 0;
}
