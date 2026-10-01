#pragma once

/**
 * TheDiscordList integration for D++.
 *
 *     dpp::cluster bot(std::getenv("DISCORD_TOKEN"));
 *     thediscordlist::autoposter poster(bot, std::getenv("TDL_TOKEN"));
 *     bot.start(dpp::st_wait);
 */

#include <dpp/dpp.h>
#include <thediscordlist/webhooks.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace thediscordlist {

inline constexpr const char* version = "0.1.0";
inline constexpr const char* default_base_url = "https://thediscordlist.com/api";

enum class publisher { stats, heartbeat, commands };

/** "stats", "heartbeat" or "commands". */
const char* to_string(publisher kind) noexcept;

struct post_result {
	bool ok = false;
	/** HTTP status, or 0 when no response was received. */
	int status = 0;
	/** Set when nothing was sent: "unchanged", "min-gap", "plan", "suspended", "empty", ... */
	std::string skipped;
};

struct publisher_error {
	publisher type;
	int status;
	bool retryable;
	std::string message;
};

struct cluster_info {
	std::uint32_t id = 0;
	std::uint32_t count = 1;
};

struct options {
	/** Discord application id. Default: bot.me.id once the first shard is ready. */
	std::string bot_id;
	std::string base_url = default_base_url;

	struct {
		bool enabled = true;
		std::chrono::seconds interval{300};
		/** Trailing debounce after guild join/leave. */
		std::chrono::seconds debounce{20};
		/** Hard floor between two stats POSTs, manual posts included. */
		std::chrono::seconds min_gap{60};
	} stats;

	struct {
		bool enabled = true;
		/** Starting cadence; retuned from the API response. */
		std::chrono::seconds interval{30};
	} heartbeat;

	struct {
		bool enabled = true;
		/** An empty list CLEARS the listing's commands, so it is not sent unless asked. */
		bool post_empty = false;
	} commands;

	/** Default: bot.cluster_id and bot.maxclusters. */
	std::optional<cluster_info> cluster;

	/** The bot-wide guild total. Required for stats when the bot runs more than one cluster. */
	std::function<std::optional<std::uint64_t>()> server_count;

	std::function<void(publisher type, int status)> on_posted;
	std::function<void(const publisher_error& error)> on_error;
	std::function<void(publisher type, int status, const std::string& reason)> on_disabled;
};

using result_callback = std::function<void(const post_result& result)>;

/**
 * Publishes server count, shard heartbeats and global commands for one dpp::cluster.
 *
 * Construct it before or after bot.start(), and keep it alive for as long as it should
 * post; it must not outlive the cluster. An empty token logs a warning and leaves an
 * inert poster. Nothing here throws into your bot: callbacks run on D++ threads,
 * failures are logged through bot.log() and reported through the options callbacks.
 */
class autoposter {
public:
	autoposter(dpp::cluster& bot, std::string token, options opts = {});
	~autoposter();

	autoposter(const autoposter&) = delete;
	autoposter& operator=(const autoposter&) = delete;

	/** Manual posts never throw, and re-arm a publisher that disabled itself. */
	void post_stats(result_callback done = {});
	void post_heartbeat(result_callback done = {});
	void post_commands(result_callback done = {});

	/** Cancels every timer and detaches every event handler. Not restartable. */
	void stop();

private:
	struct impl;
	std::unique_ptr<impl> impl_;
};

}
