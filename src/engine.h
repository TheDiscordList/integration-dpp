#pragma once

// The publisher state machines, free of any live cluster: autoposter.cpp adapts them to
// D++, and the tests drive them with a fake host.

#include <thediscordlist/dpp.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace thediscordlist::detail {

inline constexpr std::int64_t stats_first_post_delay_ms = 5'000;
/** Republish an unchanged count this often, so a listing never looks abandoned. */
inline constexpr std::int64_t stats_force_after_ms = 1'800'000;
inline constexpr std::int64_t stats_remeasure_ms = 60'000;
inline constexpr std::int64_t heartbeat_min_interval_ms = 15'000;
inline constexpr std::int64_t heartbeat_max_interval_ms = 60'000;
inline constexpr std::int64_t heartbeat_jitter_ms = 5'000;
/** Offset from the first stats post: the two share one server-side rate budget. */
inline constexpr std::int64_t commands_delay_ms = 10'000;
/** The Gold tier is derived per request, so a bot can gain it mid-run. */
inline constexpr std::int64_t plan_reprobe_ms = 1'800'000;
inline constexpr std::int64_t max_latency_ms = 600'000;
inline constexpr int max_attempts = 3;
inline constexpr const char* content_type = "application/json";

struct response {
	/** 0 when nothing came back: a network error or the 10 second timeout. */
	int status = 0;
	std::string body;
	std::string retry_after;
	std::string ratelimit_reset;
	std::string error;
};

enum class outcome_kind { ok, payload, auth, plan, config, throttled, transient, dropped };

struct outcome {
	outcome_kind kind = outcome_kind::dropped;
	int status = 0;
	std::int64_t retry_after_ms = 0;
	nlohmann::json body;
	std::string error;
};

outcome classify(const response& r, std::int64_t now_ms);
std::int64_t backoff_ms(int attempt, double random01);

struct shard_report {
	std::uint32_t id = 0;
	std::string status;
	std::optional<std::int64_t> latency_ms;
	std::uint64_t guilds = 0;
};

std::string shard_status(bool connected, bool has_session);
std::optional<std::int64_t> latency_ms(double websocket_ping_seconds);

nlohmann::json stats_body(std::uint64_t server_count, std::uint32_t shard_count);
nlohmann::json heartbeat_body(const cluster_info& cluster, const std::vector<shard_report>& shards);

struct commands_payload {
	std::optional<nlohmann::json> body;
	std::string skipped;
};

commands_payload commands_body(std::string_view discord_response, bool post_empty);

/** nullopt when the count is not one the API accepts; heartbeats must then stay off. */
std::optional<cluster_info> resolve_cluster(const std::optional<cluster_info>& configured, std::uint32_t cluster_id,
	std::uint32_t maxclusters);

std::string user_agent();
std::multimap<std::string, std::string> request_headers(const std::string& token);

class host {
public:
	virtual ~host() = default;

	virtual std::int64_t now_ms() = 0;
	virtual double random() = 0;
	virtual std::uint64_t schedule(std::int64_t delay_ms, std::function<void()> fn) = 0;
	virtual void cancel(std::uint64_t handle) = 0;
	virtual void send(const std::string& path, const std::string& body, std::function<void(const response&)> done) = 0;
	virtual std::string bot_id() = 0;
	virtual std::optional<std::uint64_t> server_count() = 0;
	virtual std::uint32_t shard_count() = 0;
	virtual std::vector<shard_report> shards() = 0;
	virtual void fetch_commands(std::function<void(std::optional<std::string> body, const std::string& error)> done) = 0;
	virtual void log(dpp::loglevel level, const std::string& message) = 0;
};

class engine : public std::enable_shared_from_this<engine> {
public:
	engine(std::unique_ptr<host> h, options opts, std::optional<cluster_info> cluster, bool elected);

	/** Before ready is fine: a booting bot should read "connecting", not dead. */
	void start_heartbeat();
	void start();
	void guild_changed();
	void beat_now();
	void post(publisher kind, result_callback done);
	void stop();

private:
	struct state {
		explicit state(publisher k) : kind(k) {
		}

		publisher kind;
		std::uint64_t timer = 0;
		std::uint64_t generation = 0;
		std::optional<std::string> disabled;
		std::int64_t plan_until = 0;
		std::set<int> reported;
		bool in_flight = false;
	};

	using tick_fn = void (engine::*)();
	using then_fn = std::function<void(const post_result&)>;

	template <class F>
	void locked(F&& body);
	void defer(std::function<void()> call);
	void log(dpp::loglevel level, const std::string& message);
	void warn_once(const std::string& key, const std::string& message);
	void finish(const result_callback& done, const post_result& result);

	void schedule(state& s, std::int64_t delay_ms, tick_fn tick);
	void clear(state& s);
	void send(state& s, const std::string& path, const std::string& body, bool heartbeat, std::int64_t next_tick_ms,
		then_fn then, int attempt = 0);
	post_result react(state& s, const outcome& o);
	void disable(state& s, int status, const std::string& reason, bool quiet = false);
	void report_once(state& s, int status, const std::string& message);
	void emit_error(state& s, int status, const std::string& message, bool retryable);

	void begin_heartbeat();
	void stats_tick();
	void publish_stats(bool manual, result_callback done);
	void heartbeat_tick();
	void beat(bool manual, result_callback done);
	void schedule_next_beat();
	void retune(const nlohmann::json& body);
	void commands_tick();
	void publish_commands(result_callback done);

	std::unique_ptr<host> host_;
	options opts_;
	std::optional<cluster_info> cluster_;
	bool elected_;

	// ponytail: one recursive lock for all three publishers; their work is a few
	// microseconds per tick, so finer locking would buy nothing.
	std::recursive_mutex mutex_;
	std::vector<std::function<void()>> outbox_;
	std::set<std::string> warned_;
	bool started_ = false;
	bool heartbeat_started_ = false;
	bool stopped_ = false;

	/** One client-wide 429 suspension: the API keeps one counter per IP across every route. */
	std::int64_t suspended_until_ = 0;

	state stats_{publisher::stats};
	state heartbeat_{publisher::heartbeat};
	state commands_{publisher::commands};

	std::int64_t last_post_at_ = 0;
	std::int64_t debounce_cap_ = 0;
	std::optional<std::uint64_t> last_count_;
	std::uint32_t last_shard_count_ = 0;
	std::int64_t heartbeat_interval_ms_;
};

}
