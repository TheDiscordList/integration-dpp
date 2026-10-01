#pragma once

#include <dpp/json.h>

#include <chrono>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace thediscordlist {

inline constexpr const char* webhook_signature_header = "X-TDL-Signature";
inline constexpr std::chrono::seconds default_webhook_tolerance{300};

enum class webhook_error_code {
	missing_signature,
	malformed_signature,
	stale_timestamp,
	invalid_signature,
	malformed_body,
};

/** "MISSING_SIGNATURE", "MALFORMED_SIGNATURE", "STALE_TIMESTAMP", "INVALID_SIGNATURE" or "MALFORMED_BODY". */
const char* to_string(webhook_error_code code) noexcept;

class webhook_error : public std::runtime_error {
public:
	webhook_error(webhook_error_code code, const std::string& message);

	webhook_error_code code() const noexcept { return code_; }

private:
	webhook_error_code code_;
};

struct webhook_listing {
	/** Guild id for a server, application id for a bot. */
	std::string id;
	/** "server" or "bot". */
	std::string type;
	std::string name;
	std::string url;
};

struct webhook_event {
	/** Equals X-TDL-Delivery and is stable across every retry: deduplicate on it. */
	std::string id;
	/** "listing.bumped", "listing.upvoted", "server.joined", "bot.added", "bot.stats_reported",
	 * "webhook.ping", or a name added later. */
	std::string event;
	/** Enqueue time, not send time. Never use it for freshness. */
	std::string created_at;
	webhook_listing listing;
	nlohmann::json data;
};

/**
 * Verifies X-TDL-Signature and returns the parsed envelope.
 *
 * raw_body must be the exact bytes that arrived. Re-serialising a parsed body changes
 * escaping and key order, so it can never verify.
 *
 * @throws webhook_error, and nothing else.
 */
webhook_event verify_webhook(std::string_view raw_body, std::string_view signature_header, std::string_view secret,
	std::chrono::seconds tolerance = default_webhook_tolerance, std::optional<std::time_t> now = std::nullopt);

}
