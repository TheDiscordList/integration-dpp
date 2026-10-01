#include "engine.h"

#include <atomic>
#include <random>
#include <shared_mutex>
#include <unordered_map>

namespace thediscordlist {

namespace {

constexpr std::uint64_t request_timeout_seconds = 10;
/** Above this, numshards is D++ 10.1's NO_SHARDS sentinel for a gateway-less cluster. */
constexpr std::uint32_t max_shards = 65536;

std::string header(const dpp::http_request_completion_t& http, const char* name) {
	auto found = http.headers.find(name);
	return found == http.headers.end() ? std::string() : found->second;
}

class dpp_host final : public detail::host {
public:
	dpp_host(dpp::cluster& bot, std::string token, const options& opts, std::optional<cluster_info> cluster)
		: bot_(bot), token_(std::move(token)), base_url_(opts.base_url), bot_id_(opts.bot_id),
		  server_count_(opts.server_count), cluster_(cluster), random_(std::random_device{}()) {
		while (!base_url_.empty() && base_url_.back() == '/') {
			base_url_.pop_back();
		}
	}

	std::int64_t now_ms() override {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	double random() override {
		std::lock_guard<std::mutex> lock(random_mutex_);
		return std::uniform_real_distribution<double>(0.0, 1.0)(random_);
	}

	// D++ timers tick once a second, so every delay rounds up to whole seconds.
	std::uint64_t schedule(std::int64_t delay_ms, std::function<void()> fn) override {
		dpp::cluster* bot = &bot_;
		auto seconds = static_cast<std::uint64_t>(std::max<std::int64_t>(1, (delay_ms + 999) / 1000));
		return bot_.start_timer([bot, fn = std::move(fn)](dpp::timer handle) {
			bot->stop_timer(handle);
			fn();
		}, seconds);
	}

	void cancel(std::uint64_t handle) override {
		bot_.stop_timer(handle);
	}

	// dpp::cluster::request has no per-request timeout (only the cluster-wide one, which
	// also governs Discord calls), so a one-shot timer races the response.
	void send(const std::string& path, const std::string& body, std::function<void(const detail::response&)> done) override {
		dpp::cluster* bot = &bot_;
		auto settled = std::make_shared<std::atomic<bool>>(false);
		auto timeout = bot_.start_timer([bot, settled, done](dpp::timer handle) {
			bot->stop_timer(handle);
			if (settled->exchange(true)) {
				return;
			}
			detail::response r;
			r.error = "No response from TheDiscordList within 10 seconds.";
			done(r);
		}, request_timeout_seconds);

		bot_.request(base_url_ + path, dpp::m_post, [bot, settled, timeout, done](const dpp::http_request_completion_t& http) {
			if (settled->exchange(true)) {
				return;
			}
			bot->stop_timer(timeout);
			detail::response r;
			if (http.error == dpp::h_success && http.status >= 100) {
				r.status = http.status;
				r.body = http.body;
				r.retry_after = header(http, "retry-after");
				r.ratelimit_reset = header(http, "x-ratelimit-reset");
			} else {
				r.error = "Could not reach TheDiscordList (D++ http_error " + std::to_string(static_cast<int>(http.error)) + ").";
			}
			done(r);
		}, body, detail::content_type, detail::request_headers(token_));
	}

	std::string bot_id() override {
		if (!bot_id_.empty()) {
			return bot_id_;
		}
		return bot_.me.id.empty() ? std::string() : bot_.me.id.str();
	}

	std::optional<std::uint64_t> server_count() override {
		if (server_count_) {
			try {
				return server_count_();
			} catch (...) {
				return std::nullopt;
			}
		}
		// The guild cache is process-wide, so it is the bot-wide total only with one cluster.
		if (cluster_ && cluster_->count == 1) {
			return dpp::get_guild_count();
		}
		return std::nullopt;
	}

	std::uint32_t shard_count() override {
		auto total = bot_.numshards;
		return total == 0 || total > max_shards ? 1 : total;
	}

	std::vector<detail::shard_report> shards() override {
		std::vector<detail::shard_report> out;
		auto total = bot_.numshards;
		if (total == 0 || total > max_shards || bot_.maxclusters == 0) {
			return out;
		}

		std::unordered_map<std::uint32_t, std::uint64_t> guilds;
		{
			auto* cache = dpp::get_guild_cache();
			std::shared_lock<std::shared_mutex> lock(cache->get_mutex());
			for (const auto& entry : cache->get_container()) {
				++guilds[entry.second->shard_id];
			}
		}

		// get_shard() takes the cluster's shard lock; iterating get_shards() would not.
		// Shards this cluster owns but has not created yet (staggered startup) read "connecting".
		for (std::uint32_t id = bot_.cluster_id; id < total; id += bot_.maxclusters) {
			detail::shard_report report;
			report.id = id;
			report.guilds = guilds[id];
			if (auto* shard = bot_.get_shard(id)) {
				report.status = detail::shard_status(shard->is_connected(), !shard->sessionid.empty());
				report.latency_ms = detail::latency_ms(shard->websocket_ping);
			} else {
				report.status = detail::shard_status(false, false);
			}
			out.push_back(std::move(report));
		}
		return out;
	}

	// The raw REST body, not dpp::slashcommand::to_json(): that one turns a null
	// default_member_permissions into "0" (admins only) and numeric choices into strings.
	void fetch_commands(std::function<void(std::optional<std::string>, const std::string&)> done) override {
		bot_.global_commands_get([done](const dpp::confirmation_callback_t& result) {
			if (result.is_error()) {
				done(std::nullopt, result.get_error().message);
				return;
			}
			done(result.http_info.body, "");
		});
	}

	void log(dpp::loglevel level, const std::string& message) override {
		bot_.log(level, message);
	}

private:
	dpp::cluster& bot_;
	std::string token_;
	std::string base_url_;
	std::string bot_id_;
	std::function<std::optional<std::uint64_t>()> server_count_;
	std::optional<cluster_info> cluster_;
	std::mutex random_mutex_;
	std::mt19937_64 random_;
};

}

struct autoposter::impl {
	explicit impl(dpp::cluster& cluster) : bot(cluster) {
	}

	dpp::cluster& bot;
	std::shared_ptr<detail::engine> engine;
	dpp::event_handle ready = 0;
	dpp::event_handle resumed = 0;
	dpp::event_handle guild_create = 0;
	dpp::event_handle guild_delete = 0;
};

autoposter::autoposter(dpp::cluster& bot, std::string token, options opts) : impl_(std::make_unique<impl>(bot)) {
	if (token.empty()) {
		bot.log(dpp::ll_warning, "[thediscordlist] No stats token supplied; auto-posting is disabled.");
		return;
	}
	if ((bot.intents & dpp::i_guilds) == 0) {
		bot.log(dpp::ll_warning, "[thediscordlist] dpp::i_guilds is missing from the intents, so the guild cache "
			"never changes and the reported server count will silently drift.");
	}

	auto cluster = detail::resolve_cluster(opts.cluster, bot.cluster_id, bot.maxclusters);
	bool elected = (opts.cluster ? opts.cluster->id : bot.cluster_id) == 0;
	bool has_bot_id = !opts.bot_id.empty();

	auto host = std::make_unique<dpp_host>(bot, std::move(token), opts, cluster);
	impl_->engine = std::make_shared<detail::engine>(std::move(host), std::move(opts), cluster, elected);
	std::weak_ptr<detail::engine> weak = impl_->engine;

	// With an explicit bot id the heartbeat starts now, so a booting bot reads "connecting".
	if (has_bot_id) {
		impl_->engine->start_heartbeat();
	}

	impl_->ready = bot.on_ready([weak](const dpp::ready_t&) {
		if (auto engine = weak.lock()) {
			engine->start();
			engine->beat_now();
		}
	});
	impl_->resumed = bot.on_resumed([weak](const dpp::resumed_t&) {
		if (auto engine = weak.lock()) {
			engine->beat_now();
		}
	});
	impl_->guild_create = bot.on_guild_create([weak](const dpp::guild_create_t&) {
		if (auto engine = weak.lock()) {
			engine->guild_changed();
		}
	});
	impl_->guild_delete = bot.on_guild_delete([weak](const dpp::guild_delete_t&) {
		if (auto engine = weak.lock()) {
			engine->guild_changed();
		}
	});

	// Constructed after the first READY: on_ready will not fire again until a reconnect.
	if (!bot.me.id.empty()) {
		impl_->engine->start();
	}
}

autoposter::~autoposter() {
	stop();
}

void autoposter::stop() {
	if (!impl_->engine) {
		return;
	}
	impl_->bot.on_ready.detach(impl_->ready);
	impl_->bot.on_resumed.detach(impl_->resumed);
	impl_->bot.on_guild_create.detach(impl_->guild_create);
	impl_->bot.on_guild_delete.detach(impl_->guild_delete);
	impl_->engine->stop();
}

namespace {

// By value: a callback that destroys the poster must not destroy the engine mid-call.
void post(std::shared_ptr<detail::engine> engine, publisher kind, result_callback done) {
	if (engine) {
		engine->post(kind, std::move(done));
		return;
	}
	if (done) {
		try {
			done({false, 0, "disabled"});
		} catch (...) {
		}
	}
}

}

void autoposter::post_stats(result_callback done) {
	post(impl_->engine, publisher::stats, std::move(done));
}

void autoposter::post_heartbeat(result_callback done) {
	post(impl_->engine, publisher::heartbeat, std::move(done));
}

void autoposter::post_commands(result_callback done) {
	post(impl_->engine, publisher::commands, std::move(done));
}

}
