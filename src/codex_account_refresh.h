#pragma once

#include "codex_current_account.h"
#include "codex_quota_exchange.h"

namespace codex {

// One fresh native process; only sanitized observations cross the helper pipe.
class Account_refresh_exchange
{
public:
    bool consume(std::string_view bytes);
    bool finish();
    std::string take_input();
    std::string take_output();
    bool close_input();

private:
    bool record(std::string_view line);

    bool m_failed = false;
    std::size_t m_output_bytes = 0;
    std::string m_pending;
    std::string m_output;
    Quota_exchange m_quota;
    std::optional<Current_account_exchange> m_auth;
};

} // namespace codex
