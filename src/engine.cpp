#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace thediscordlist {

const char* to_string(publisher kind) noexcept {
	switch (kind) {
		case publisher::stats: return "stats";
		case publisher::heartbeat: return "heartbeat";
		case publisher::commands: return "commands";
	}
	return "unknown";
}

}

namespace thediscordlist::detail {

namespace {

std::optional<double> parse_number(const std::string& text) {
	if (text.empty()) {
		return std::nullopt;
	}
	char* end = nullptr;
	double value = std::strtod(text.c_str(), &end);
	if (end != text.c_str() + text.size() || !std::isfinite(value)) {
		return std::nullopt;
	}
	return value;
}

std::int64_t to_ms(std::chrono::seconds seconds) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(seconds).count();
}

post_result skipped(const char* reason) {
	return {false, 0, reason};
}

std::string explain(const std::string& reason, publisher kind) {
	if (reason == "not-found") {
		return "TheDiscordList does not know this bot id, or it belongs to a server listing (404). Check options.bot_id; "
			"the stats token is not the problem.";
	}
	if (reason == "conflict") {
		return "The listing is not configured for slash commands (409).";
	}
	if (reason == "cluster-count-unknown") {
		return "Heartbeats are off: the cluster count is not between 1 and 1024 with id < count, and a wrong "
			"cluster_count deletes sibling clusters server-side. Set options.cluster = {id, count}.";
	}
	return std::string("The ") + to_string(kind) + " publisher is disabled (" + reason + ").";
}

}

outcome classify(const response& r, std::int64_t now_ms) {
	outcome o;
	o.status = r.status;
	if (r.status >= 200 && r.status < 300) {
		o.kind = outcome_kind::ok;
		o.body = nlohmann::json::parse(r.body, nullptr, false);
		if (o.body.is_discarded()) {
			o.body = nullptr;
		}
		return o;
	}
	switch (r.status) {
		case 401:
			o.kind = outcome_kind::auth;
			return o;
		case 403:
			o.kind = outcome_kind::plan;
			return o;
		// 404 is "unknown id, or the id belongs to a server listing", never "bad token".
		case 404:
		case 409:
			o.kind = outcome_kind::config;
			return o;
		case 400:
		case 422:
			o.kind = outcome_kind::payload;
			return o;
		case 429: {
			o.kind = outcome_kind::throttled;
			auto seconds = parse_number(r.retry_after);
			o.retry_after_ms = seconds && *seconds > 0 ? std::llround(*seconds * 1000) : 60'000;
			auto reset = parse_number(r.ratelimit_reset);
			if (reset && *reset > 0) {
				o.retry_after_ms = std::max<std::int64_t>(o.retry_after_ms, std::llround(*reset * 1000) - now_ms);
			}
			return o;
		}
		default:
			break;
	}
	o.kind = outcome_kind::transient;
	o.error = r.status == 0
		? (r.error.empty() ? std::string("Could not reach TheDiscordList.") : r.error)
		: "TheDiscordList responded " + std::to_string(r.status) + ".";
	return o;
}

std::int64_t backoff_ms(int attempt, double random01) {
	double base = std::min(1000.0 * std::pow(2.0, attempt), 30'000.0);
	return std::llround(base * (0.5 + random01 * 0.5));
}

std::string shard_status(bool connected, bool has_session) {
	if (connected) {
		return "ready";
	}
	// D++ resumes a dropped shard on its own, keeping the session id while it does.
	return has_session ? "resuming" : "connecting";
}

std::optional<std::int64_t> latency_ms(double websocket_ping_seconds) {
	// D++ reports 0 until the first heartbeat ACK; the key is omitted rather than sent as 0.
	if (!(websocket_ping_seconds > 0) || !std::isfinite(websocket_ping_seconds)) {
		return std::nullopt;
	}
	return std::min<std::int64_t>(std::llround(websocket_ping_seconds * 1000), max_latency_ms);
}

nlohmann::json stats_body(std::uint64_t server_count, std::uint32_t shard_count) {
	// shard_count is always sent: the server stores `shard_count ?? null`, so omitting it erases it.
	return {{"server_count", server_count}, {"shard_count", std::max<std::uint32_t>(shard_count, 1)}};
}

nlohmann::json heartbeat_body(const cluster_info& cluster, const std::vector<shard_report>& shards) {
	auto list = nlohmann::json::array();
	for (const auto& shard : shards) {
		nlohmann::json entry = {{"id", shard.id}, {"status", shard.status}, {"guilds", shard.guilds}};
		if (shard.latency_ms) {
			entry["latency_ms"] = *shard.latency_ms;
		}
		list.push_back(std::move(entry));
	}
	return {{"cluster", cluster.id}, {"cluster_count", cluster.count}, {"shards", std::move(list)}};
}

commands_payload commands_body(std::string_view discord_response, bool post_empty) {
	auto commands = nlohmann::json::parse(discord_response.begin(), discord_response.end(), nullptr, false);
	if (commands.is_discarded() || !commands.is_array()) {
		return {std::nullopt, "fetch-failed"};
	}
	if (commands.empty() && !post_empty) {
		return {std::nullopt, "empty"};
	}
	// Discord's own shape, forwarded untouched: the API accepts it as-is, nulls included.
	return {nlohmann::json{{"commands", std::move(commands)}}, ""};
}

std::optional<cluster_info> resolve_cluster(const std::optional<cluster_info>& configured, std::uint32_t cluster_id,
	std::uint32_t maxclusters) {
	cluster_info cluster = configured ? *configured : cluster_info{cluster_id, maxclusters};
	if (cluster.count < 1 || cluster.count > 1024 || cluster.id >= cluster.count) {
		return std::nullopt;
	}
	return cluster;
}

std::string user_agent() {
	return std::string("TheDiscordList-dpp/") + version + " (+https://github.com/TheDiscordList/integration-dpp)";
}

std::multimap<std::string, std::string> request_headers(const std::string& token) {
	// Content-Type travels as the mimetype argument of dpp::cluster::request.
	return {{"Authorization", "Bearer " + token}, {"User-Agent", user_agent()}};
}

engine::engine(std::unique_ptr<host> h, options opts, std::optional<cluster_info> cluster, bool elected)
	: host_(std::move(h)), opts_(std::move(opts)), cluster_(cluster), elected_(elected),
	  heartbeat_interval_ms_(std::clamp(to_ms(opts_.heartbeat.interval), heartbeat_min_interval_ms, heartbeat_max_interval_ms)) {
}

template <class F>
void engine::locked(F&& body) {
	std::vector<std::function<void()>> calls;
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		try {
			body();
		} catch (const std::exception& e) {
			outbox_.emplace_back([this, what = std::string(e.what())] {
				host_->log(dpp::ll_error, "[thediscordlist] Unexpected error: " + what);
			});
		} catch (...) {
			outbox_.emplace_back([this] { host_->log(dpp::ll_error, "[thediscordlist] Unexpected error."); });
		}
		calls.swap(outbox_);
	}
	// User callbacks run outside the lock, and nothing they throw reaches D++.
	for (auto& call : calls) {
		try {
			call();
		} catch (const std::exception& e) {
			host_->log(dpp::ll_error, std::string("[thediscordlist] A callback threw: ") + e.what());
		} catch (...) {
			host_->log(dpp::ll_error, "[thediscordlist] A callback threw.");
		}
	}
}

void engine::defer(std::function<void()> call) {
	outbox_.push_back(std::move(call));
}

void engine::log(dpp::loglevel level, const std::string& message) {
	defer([this, level, message] { host_->log(level, "[thediscordlist] " + message); });
}

void engine::warn_once(const std::string& key, const std::string& message) {
	if (warned_.insert(key).second) {
		log(dpp::ll_warning, message);
	}
}

void engine::finish(const result_callback& done, const post_result& result) {
	if (done) {
		defer([done, result] { done(result); });
	}
}

void engine::schedule(state& s, std::int64_t delay_ms, tick_fn tick) {
	if (stopped_) {
		return;
	}
	clear(s);
	std::weak_ptr<engine> weak = weak_from_this();
	state* target = &s;
	auto generation = s.generation;
	s.timer = host_->schedule(std::max<std::int64_t>(delay_ms, 0), [weak, target, generation, tick] {
		if (auto self = weak.lock()) {
			self->locked([&] {
				if (target->generation != generation) {
					return;
				}
				target->timer = 0;
				(self.get()->*tick)();
			});
		}
	});
}

void engine::clear(state& s) {
	if (s.timer != 0) {
		host_->cancel(s.timer);
	}
	s.timer = 0;
	++s.generation;
}

void engine::send(state& s, const std::string& path, const std::string& body, bool heartbeat, std::int64_t next_tick_ms,
	then_fn then, int attempt) {
	// Heartbeats bypass the suspension: a dropped stats post is a stale number, a dropped
	// heartbeat is a false "down" on the public status page.
	if (!heartbeat && host_->now_ms() < suspended_until_) {
		then(react(s, outcome{}));
		return;
	}

	std::weak_ptr<engine> weak = weak_from_this();
	state* target = &s;
	host_->send(path, body, [weak, target, path, body, heartbeat, next_tick_ms, then, attempt](const response& r) {
		auto self = weak.lock();
		if (!self) {
			return;
		}
		self->locked([&] {
			auto now = self->host_->now_ms();
			auto result = classify(r, now);
			if (result.kind == outcome_kind::throttled) {
				self->suspended_until_ = now + result.retry_after_ms + std::llround(self->host_->random() * 2000);
			}
			if (result.kind == outcome_kind::transient && attempt + 1 < max_attempts) {
				auto delay = backoff_ms(attempt, self->host_->random());
				// A retry that would outlast the next tick is dropped: that tick carries fresher data.
				if (next_tick_ms <= 0 || delay < next_tick_ms) {
					self->host_->schedule(delay, [weak, target, path, body, heartbeat, next_tick_ms, then, attempt] {
						if (auto again = weak.lock()) {
							again->locked([&] {
								again->send(*target, path, body, heartbeat, next_tick_ms, then, attempt + 1);
							});
						}
					});
					return;
				}
			}
			then(self->react(*target, result));
		});
	});
}

post_result engine::react(state& s, const outcome& o) {
	switch (o.kind) {
		case outcome_kind::ok:
			if (s.kind == publisher::heartbeat) {
				retune(o.body);
			}
			if (opts_.on_posted) {
				defer([cb = opts_.on_posted, kind = s.kind, status = o.status] { cb(kind, status); });
			}
			return {true, o.status, ""};

		case outcome_kind::dropped:
			return skipped("suspended");

		case outcome_kind::payload:
			// The publisher stays enabled: the next tick may carry valid data.
			report_once(s, o.status, std::string("TheDiscordList rejected the ") + to_string(s.kind) + " payload (" +
				std::to_string(o.status) + ").");
			return {false, o.status, ""};

		case outcome_kind::auth:
			log(dpp::ll_error, "TheDiscordList rejected the stats token (401). Every publisher is disabled until a "
				"manual post re-arms it.");
			for (auto* each : {&stats_, &heartbeat_, &commands_}) {
				disable(*each, 401, "unauthorized", true);
			}
			return {false, 401, ""};

		case outcome_kind::plan:
			// Not a disable: the plan can change mid-run, and noticing must not need a restart.
			s.plan_until = host_->now_ms() + plan_reprobe_ms;
			report_once(s, 403, std::string("The ") + to_string(s.kind) +
				" endpoint needs a higher plan (heartbeats need Gold). Reprobing every 30 minutes.");
			return {false, 403, "plan"};

		case outcome_kind::config:
			disable(s, o.status, o.status == 409 ? "conflict" : "not-found");
			return {false, o.status, ""};

		case outcome_kind::throttled:
			return {false, 429, "throttled"};

		case outcome_kind::transient:
			emit_error(s, o.status, o.error, true);
			return {false, o.status, ""};
	}
	return {};
}

void engine::disable(state& s, int status, const std::string& reason, bool quiet) {
	clear(s);
	if (s.disabled) {
		return;
	}
	s.disabled = reason;
	if (!quiet) {
		log(dpp::ll_warning, explain(reason, s.kind));
	}
	if (opts_.on_disabled) {
		defer([cb = opts_.on_disabled, kind = s.kind, status, reason] { cb(kind, status, reason); });
	}
}

void engine::report_once(state& s, int status, const std::string& message) {
	if (s.reported.insert(status).second) {
		emit_error(s, status, message, false);
	}
}

void engine::emit_error(state& s, int status, const std::string& message, bool retryable) {
	log(dpp::ll_warning, std::string(to_string(s.kind)) + ": " + message);
	if (opts_.on_error) {
		defer([cb = opts_.on_error, error = publisher_error{s.kind, status, retryable, message}] { cb(error); });
	}
}

void engine::start_heartbeat() {
	locked([&] { begin_heartbeat(); });
}

void engine::begin_heartbeat() {
	if (stopped_ || heartbeat_started_ || !opts_.heartbeat.enabled) {
		return;
	}
	heartbeat_started_ = true;
	// Every beat deletes the cluster rows numbered >= cluster_count, with their history.
	// A guessed count would wipe the siblings, so an unknown one means silence.
	if (!cluster_) {
		disable(heartbeat_, 0, "cluster-count-unknown");
		return;
	}
	schedule(heartbeat_, 0, &engine::heartbeat_tick);
}

void engine::start() {
	locked([&] {
		if (stopped_ || started_) {
			return;
		}
		started_ = true;
		if (cluster_) {
			log(dpp::ll_info, "Publishing as cluster " + std::to_string(cluster_->id) + " of " +
				std::to_string(cluster_->count) + ".");
		}
		begin_heartbeat();
		if (opts_.stats.enabled && elected_) {
			schedule(stats_, stats_first_post_delay_ms, &engine::stats_tick);
		}
		if (opts_.commands.enabled && elected_) {
			schedule(commands_, commands_delay_ms, &engine::commands_tick);
		}
	});
}

void engine::guild_changed() {
	locked([&] {
		if (!started_ || !opts_.stats.enabled || !elected_ || stats_.disabled) {
			return;
		}
		// Trailing debounce, hard-capped at min_gap so a join storm cannot starve the post.
		auto now = host_->now_ms();
		if (debounce_cap_ == 0) {
			debounce_cap_ = now + to_ms(opts_.stats.min_gap);
		}
		schedule(stats_, std::min(now + to_ms(opts_.stats.debounce), debounce_cap_) - now, &engine::stats_tick);
	});
}

void engine::beat_now() {
	locked([&] {
		if (!heartbeat_started_ || heartbeat_.disabled || host_->now_ms() < heartbeat_.plan_until) {
			return;
		}
		schedule(heartbeat_, 1000, &engine::heartbeat_tick);
	});
}

void engine::post(publisher kind, result_callback done) {
	locked([&] {
		switch (kind) {
			case publisher::stats:
				stats_.disabled.reset();
				publish_stats(true, std::move(done));
				break;
			case publisher::heartbeat:
				heartbeat_.disabled.reset();
				beat(true, std::move(done));
				break;
			case publisher::commands:
				commands_.disabled.reset();
				publish_commands(std::move(done));
				break;
		}
	});
}

void engine::stop() {
	locked([&] {
		stopped_ = true;
		clear(stats_);
		clear(heartbeat_);
		clear(commands_);
	});
}

void engine::stats_tick() {
	publish_stats(false, {});
}

void engine::publish_stats(bool manual, result_callback done) {
	debounce_cap_ = 0;
	if (!manual && stats_.disabled) {
		return finish(done, skipped("disabled"));
	}
	if (!elected_) {
		return finish(done, skipped("not-elected"));
	}
	if (stats_.in_flight) {
		return finish(done, skipped("in-flight"));
	}

	auto now = host_->now_ms();
	auto interval = to_ms(opts_.stats.interval);
	auto min_gap = to_ms(opts_.stats.min_gap);
	if (!manual && now < stats_.plan_until) {
		schedule(stats_, stats_.plan_until - now, &engine::stats_tick);
		return finish(done, skipped("plan"));
	}
	// A hard floor that manual posts, join storms and mass reconnects all route through.
	if (last_post_at_ != 0 && now < last_post_at_ + min_gap) {
		schedule(stats_, last_post_at_ + min_gap - now, &engine::stats_tick);
		return finish(done, skipped("min-gap"));
	}

	auto bot_id = host_->bot_id();
	if (bot_id.empty()) {
		warn_once("no-bot-id", "The bot id is not known yet. Pass options.bot_id, or wait for the first shard to be ready.");
		schedule(stats_, stats_remeasure_ms, &engine::stats_tick);
		return finish(done, skipped("no-bot-id"));
	}
	auto count = host_->server_count();
	if (!count) {
		warn_once("unmeasurable", "Could not measure the server count. A bot running more than one cluster must pass "
			"options.server_count returning the bot-wide total.");
		schedule(stats_, stats_remeasure_ms, &engine::stats_tick);
		return finish(done, skipped("unmeasurable"));
	}

	auto shard_count = host_->shard_count();
	// Every stats post re-indexes the listing and fires a webhook, even for the same number.
	if (count == last_count_ && shard_count == last_shard_count_ && now < last_post_at_ + stats_force_after_ms) {
		schedule(stats_, interval, &engine::stats_tick);
		return finish(done, skipped("unchanged"));
	}

	last_post_at_ = now;
	stats_.in_flight = true;
	send(stats_, "/bots/" + bot_id + "/stats", stats_body(*count, shard_count).dump(), false, interval,
		[this, count, shard_count, interval, done](const post_result& result) {
			stats_.in_flight = false;
			if (result.ok) {
				last_count_ = count;
				last_shard_count_ = shard_count;
			}
			if (started_ && opts_.stats.enabled && !stats_.disabled) {
				schedule(stats_, interval, &engine::stats_tick);
			}
			finish(done, result);
		});
}

void engine::heartbeat_tick() {
	beat(false, {});
}

void engine::beat(bool manual, result_callback done) {
	if (!cluster_) {
		return finish(done, skipped("cluster-count-unknown"));
	}
	if (!manual && heartbeat_.disabled) {
		return finish(done, skipped("disabled"));
	}
	if (heartbeat_.in_flight) {
		return finish(done, skipped("in-flight"));
	}

	auto bot_id = host_->bot_id();
	auto shards = bot_id.empty() ? std::vector<shard_report>{} : host_->shards();
	// Before login. The API requires at least one shard.
	if (shards.empty()) {
		if (!manual && heartbeat_started_) {
			schedule_next_beat();
		}
		return finish(done, skipped(bot_id.empty() ? "no-bot-id" : "no-shards"));
	}

	heartbeat_.in_flight = true;
	send(heartbeat_, "/bots/" + bot_id + "/heartbeat", heartbeat_body(*cluster_, shards).dump(), true,
		heartbeat_interval_ms_, [this, manual, done](const post_result& result) {
			heartbeat_.in_flight = false;
			// A manual beat revives a loop that died with a disable.
			if (heartbeat_started_ && !heartbeat_.disabled && (!manual || heartbeat_.timer == 0)) {
				schedule_next_beat();
			}
			finish(done, result);
		});
}

void engine::schedule_next_beat() {
	std::int64_t delay = heartbeat_interval_ms_ + std::llround(host_->random() * heartbeat_jitter_ms);
	schedule(heartbeat_, std::max(delay, heartbeat_.plan_until - host_->now_ms()), &engine::heartbeat_tick);
}

void engine::retune(const nlohmann::json& body) {
	// The cadence follows next_beat_within_seconds, so a server-side change to the grace
	// window retunes existing installs without a release.
	if (!body.is_object()) {
		return;
	}
	auto seconds = body.find("next_beat_within_seconds");
	if (seconds == body.end() || !seconds->is_number() || seconds->get<double>() <= 0) {
		return;
	}
	heartbeat_interval_ms_ = std::clamp<std::int64_t>(std::llround(seconds->get<double>() * 1000 / 3),
		heartbeat_min_interval_ms, heartbeat_max_interval_ms);
}

void engine::commands_tick() {
	publish_commands({});
}

void engine::publish_commands(result_callback done) {
	if (commands_.disabled) {
		return finish(done, skipped("disabled"));
	}
	if (!elected_) {
		return finish(done, skipped("not-elected"));
	}
	if (host_->now_ms() < commands_.plan_until) {
		return finish(done, skipped("plan"));
	}
	if (commands_.in_flight) {
		return finish(done, skipped("in-flight"));
	}
	auto bot_id = host_->bot_id();
	if (bot_id.empty()) {
		return finish(done, skipped("no-bot-id"));
	}

	commands_.in_flight = true;
	std::weak_ptr<engine> weak = weak_from_this();
	host_->fetch_commands([weak, bot_id, done](std::optional<std::string> raw, const std::string& error) {
		auto self = weak.lock();
		if (!self) {
			return;
		}
		self->locked([&] {
			auto& s = self->commands_;
			s.in_flight = false;
			if (!raw) {
				self->emit_error(s, 0, "Could not fetch the global application commands from Discord: " + error, false);
				return self->finish(done, skipped("fetch-failed"));
			}
			auto payload = commands_body(*raw, self->opts_.commands.post_empty);
			if (!payload.body) {
				if (payload.skipped == "fetch-failed") {
					self->emit_error(s, 0, "Discord returned an unreadable command list.", false);
				}
				return self->finish(done, {false, 0, payload.skipped});
			}
			s.in_flight = true;
			auto* engine = self.get();
			self->send(s, "/bots/" + bot_id + "/commands", payload.body->dump(), false, 0,
				[engine, done](const post_result& result) {
					engine->commands_.in_flight = false;
					engine->finish(done, result);
				});
		});
	});
}

}
