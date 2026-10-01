#include "engine.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

using namespace thediscordlist;
using nlohmann::json;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                                       \
	do {                                                                                       \
		++checks;                                                                              \
		if (!(condition)) {                                                                    \
			++failures;                                                                        \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
		}                                                                                      \
	} while (0)

// ---------------------------------------------------------------------------------------
// Webhooks

const std::string vector_secret = "whsec_test";
const std::time_t vector_t = 1700000000;
const std::string vector_body =
	R"({"id":"d1","event":"webhook.ping","created_at":"2023-11-14T22:13:20+00:00","listing":{"id":"1","type":"bot","name":"Bot","url":"https://thediscordlist.com/bots/1"},"data":{"message":"hi"}})";
const std::string vector_v1 = "b6eeea31d04c3f86990dcbb706dfaf988c9ff2fc234316ae77e168831127dc59";

std::string sign(const std::string& secret, const std::string& t, const std::string& body) {
	std::string message = t + "." + body;
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int length = 0;
	HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
		reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &length);
	std::string hex;
	char byte[3];
	for (unsigned int i = 0; i < length; ++i) {
		std::snprintf(byte, sizeof byte, "%02x", digest[i]);
		hex += byte;
	}
	return hex;
}

std::optional<webhook_error_code> verify_error(const std::string& body, const std::string& header,
	const std::string& secret = vector_secret, std::time_t now = vector_t) {
	try {
		verify_webhook(body, header, secret, default_webhook_tolerance, now);
		return std::nullopt;
	} catch (const webhook_error& e) {
		return e.code();
	}
}

void test_webhooks() {
	auto header = "t=1700000000,v1=" + vector_v1;

	// The fixed vector, from our own HMAC and from the server's PHP formula.
	CHECK(sign(vector_secret, "1700000000", vector_body) == vector_v1);

	auto event = verify_webhook(vector_body, header, vector_secret, default_webhook_tolerance, vector_t);
	CHECK(event.id == "d1");
	CHECK(event.event == "webhook.ping");
	CHECK(event.created_at == "2023-11-14T22:13:20+00:00");
	CHECK(event.listing.id == "1");
	CHECK(event.listing.type == "bot");
	CHECK(event.listing.name == "Bot");
	CHECK(event.listing.url == "https://thediscordlist.com/bots/1");
	CHECK(event.data["message"] == "hi");

	CHECK(!verify_error(vector_body, header));
	CHECK(verify_error(vector_body + " ", header) == webhook_error_code::invalid_signature);
	CHECK(verify_error(R"({"id":"d2"})", header) == webhook_error_code::invalid_signature);
	CHECK(verify_error(vector_body, header, "whsec_wrong") == webhook_error_code::invalid_signature);

	// Two-sided window, inclusive at the edge.
	CHECK(!verify_error(vector_body, header, vector_secret, vector_t + 300));
	CHECK(!verify_error(vector_body, header, vector_secret, vector_t - 300));
	CHECK(verify_error(vector_body, header, vector_secret, vector_t + 301) == webhook_error_code::stale_timestamp);
	CHECK(verify_error(vector_body, header, vector_secret, vector_t - 301) == webhook_error_code::stale_timestamp);

	CHECK(verify_error(vector_body, "") == webhook_error_code::missing_signature);
	CHECK(verify_error(vector_body, "   ") == webhook_error_code::missing_signature);
	CHECK(verify_error(vector_body, "garbage") == webhook_error_code::malformed_signature);
	CHECK(verify_error(vector_body, "v1=" + vector_v1) == webhook_error_code::malformed_signature);
	CHECK(verify_error(vector_body, "t=1700000000") == webhook_error_code::malformed_signature);
	CHECK(verify_error(vector_body, "t=1700000000,v1=") == webhook_error_code::malformed_signature);
	CHECK(verify_error(vector_body, "t=abc,v1=" + vector_v1) == webhook_error_code::malformed_signature);
	CHECK(verify_error(vector_body, "t=,v1=" + vector_v1) == webhook_error_code::malformed_signature);
	CHECK(verify_error(vector_body, "t=inf,v1=" + vector_v1) == webhook_error_code::malformed_signature);

	// Parts split on their first '=' only, so extra '=' never breaks the parse.
	CHECK(!verify_error(vector_body, "t=1700000000,x=a=b==,v1=" + vector_v1 + ",v0=c=d"));
	CHECK(verify_error(vector_body, "t=1700000000,v1=" + vector_v1 + "=") == webhook_error_code::invalid_signature);
	// First t wins; first non-empty v1 wins.
	CHECK(!verify_error(vector_body, "t=1700000000,t=1,v1=,v1=" + vector_v1 + ",v1=deadbeef"));
	CHECK(!verify_error(vector_body, " t=1700000000 , v1=" + vector_v1 + " "));

	std::string upper = vector_v1;
	for (auto& c : upper) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	CHECK(!verify_error(vector_body, "t=1700000000,v1=" + upper));
	CHECK(verify_error(vector_body, "t=1700000000,v1=" + vector_v1.substr(1)) == webhook_error_code::invalid_signature);

	std::string not_json = "not json";
	CHECK(verify_error(not_json, "t=1700000000,v1=" + sign(vector_secret, "1700000000", not_json)) ==
		webhook_error_code::malformed_body);

	// Unknown event names are forward compatible.
	std::string future = R"({"id":"d3","event":"listing.reviewed","created_at":"x","listing":{"id":"9","type":"server","name":"S","url":"u"},"data":{"stars":5}})";
	auto parsed = verify_webhook(future, "t=1700000000,v1=" + sign(vector_secret, "1700000000", future),
		vector_secret, default_webhook_tolerance, vector_t);
	CHECK(parsed.event == "listing.reviewed");
	CHECK(parsed.data["stars"] == 5);

	CHECK(std::string(to_string(webhook_error_code::invalid_signature)) == "INVALID_SIGNATURE");
}

// ---------------------------------------------------------------------------------------
// Pure payload shaping and classification

void test_shaping() {
	CHECK(detail::stats_body(42, 2).dump() == R"({"server_count":42,"shard_count":2})");
	CHECK(detail::stats_body(0, 0)["shard_count"] == 1);

	CHECK(detail::shard_status(true, true) == "ready");
	CHECK(detail::shard_status(true, false) == "ready");
	CHECK(detail::shard_status(false, true) == "resuming");
	CHECK(detail::shard_status(false, false) == "connecting");

	CHECK(!detail::latency_ms(0.0));
	CHECK(!detail::latency_ms(-1.0));
	CHECK(detail::latency_ms(0.0426) == 43);
	CHECK(detail::latency_ms(9999.0) == detail::max_latency_ms);

	auto beat = detail::heartbeat_body({1, 4}, {{1, "ready", 43, 120}, {5, "connecting", std::nullopt, 0}});
	CHECK(beat["cluster"] == 1);
	CHECK(beat["cluster_count"] == 4);
	CHECK(beat["shards"].size() == 2);
	CHECK(beat["shards"][0] == json::parse(R"({"id":1,"status":"ready","latency_ms":43,"guilds":120})"));
	CHECK(!beat["shards"][1].contains("latency_ms"));
	CHECK(beat["shards"][1]["status"] == "connecting");

	auto raw = R"([{"id":"1","name":"ping","description":"Pong","default_member_permissions":null,"options":[{"type":4,"name":"n","description":"d","choices":[{"name":"one","value":1}]}]}])";
	auto commands = detail::commands_body(raw, false);
	CHECK(commands.body.has_value());
	CHECK((*commands.body)["commands"] == json::parse(raw));
	CHECK((*commands.body)["commands"][0]["default_member_permissions"].is_null());
	CHECK(detail::commands_body("[]", false).skipped == "empty");
	CHECK(detail::commands_body("[]", true).body == json::parse(R"({"commands":[]})"));
	CHECK(detail::commands_body("{}", true).skipped == "fetch-failed");
	CHECK(detail::commands_body("<html>", true).skipped == "fetch-failed");

	auto headers = detail::request_headers("tok");
	CHECK(headers.find("Authorization")->second == "Bearer tok");
	CHECK(headers.find("User-Agent")->second ==
		"TheDiscordList-dpp/" + std::string(version) + " (+https://github.com/TheDiscordList/integration-dpp)");
	CHECK(std::string(detail::content_type) == "application/json");
	CHECK(std::string(version) == THEDISCORDLIST_DPP_PROJECT_VERSION);

	CHECK(detail::resolve_cluster(std::nullopt, 0, 1)->count == 1);
	CHECK(detail::resolve_cluster(std::nullopt, 2, 3)->id == 2);
	CHECK(detail::resolve_cluster(cluster_info{1, 4}, 0, 1)->count == 4);
	CHECK(!detail::resolve_cluster(std::nullopt, 0, 0));
	CHECK(!detail::resolve_cluster(cluster_info{4, 4}, 0, 1));
	CHECK(!detail::resolve_cluster(cluster_info{0, 1025}, 0, 1));
}

detail::response reply(int status, std::string body = "", std::string retry_after = "", std::string reset = "") {
	detail::response r;
	r.status = status;
	r.body = std::move(body);
	r.retry_after = std::move(retry_after);
	r.ratelimit_reset = std::move(reset);
	return r;
}

void test_classify() {
	using detail::outcome_kind;
	const std::int64_t now = 1'700'000'000'000;

	auto ok = detail::classify(reply(200, R"({"next_beat_within_seconds":90})"), now);
	CHECK(ok.kind == outcome_kind::ok);
	CHECK(ok.body["next_beat_within_seconds"] == 90);
	CHECK(detail::classify(reply(204), now).kind == outcome_kind::ok);
	CHECK(detail::classify(reply(401), now).kind == outcome_kind::auth);
	CHECK(detail::classify(reply(403), now).kind == outcome_kind::plan);
	CHECK(detail::classify(reply(404), now).kind == outcome_kind::config);
	CHECK(detail::classify(reply(409), now).kind == outcome_kind::config);
	CHECK(detail::classify(reply(400), now).kind == outcome_kind::payload);
	CHECK(detail::classify(reply(422), now).kind == outcome_kind::payload);
	CHECK(detail::classify(reply(500), now).kind == outcome_kind::transient);
	CHECK(detail::classify(reply(503), now).kind == outcome_kind::transient);
	CHECK(detail::classify(reply(0), now).kind == outcome_kind::transient);

	CHECK(detail::classify(reply(429, "", "7"), now).retry_after_ms == 7'000);
	CHECK(detail::classify(reply(429), now).retry_after_ms == 60'000);
	CHECK(detail::classify(reply(429, "", "junk"), now).retry_after_ms == 60'000);
	CHECK(detail::classify(reply(429, "", "7", std::to_string(now / 1000 + 90)), now).retry_after_ms == 90'000);
	CHECK(detail::classify(reply(429, "", "120", std::to_string(now / 1000 + 5)), now).retry_after_ms == 120'000);

	CHECK(detail::backoff_ms(0, 0.0) == 500);
	CHECK(detail::backoff_ms(0, 1.0) == 1000);
	CHECK(detail::backoff_ms(2, 1.0) == 4000);
	CHECK(detail::backoff_ms(10, 1.0) == 30'000);
	CHECK(detail::backoff_ms(10, 0.0) == 15'000);
}

// ---------------------------------------------------------------------------------------
// Publisher state machines, driven through a fake host

struct fake_host final : detail::host {
	struct timer {
		std::uint64_t id;
		std::int64_t due;
		std::function<void()> fn;
		bool done = false;
	};
	struct request {
		std::string path;
		json body;
		std::function<void(const detail::response&)> done;
	};

	std::int64_t now = 1'000'000;
	std::uint64_t next_id = 1;
	std::vector<timer> timers;
	std::vector<request> requests;
	std::vector<std::function<void(std::optional<std::string>, const std::string&)>> fetches;
	std::string id = "123";
	std::optional<std::uint64_t> count = 42;
	std::vector<detail::shard_report> shard_list = {{0, "ready", 40, 42}};

	std::int64_t now_ms() override { return now; }
	double random() override { return 0.0; }
	std::uint64_t schedule(std::int64_t delay_ms, std::function<void()> fn) override {
		timers.push_back({next_id, now + delay_ms, std::move(fn)});
		return next_id++;
	}
	void cancel(std::uint64_t handle) override {
		for (auto& t : timers) {
			if (t.id == handle) {
				t.done = true;
			}
		}
	}
	void send(const std::string& path, const std::string& body, std::function<void(const detail::response&)> done) override {
		requests.push_back({path, json::parse(body), std::move(done)});
	}
	std::string bot_id() override { return id; }
	std::optional<std::uint64_t> server_count() override { return count; }
	std::uint32_t shard_count() override { return 2; }
	std::vector<detail::shard_report> shards() override { return shard_list; }
	void fetch_commands(std::function<void(std::optional<std::string>, const std::string&)> done) override {
		fetches.push_back(std::move(done));
	}
	void log(dpp::loglevel, const std::string&) override {}

	void advance(std::int64_t ms) {
		auto until = now + ms;
		for (;;) {
			timer* next = nullptr;
			for (auto& t : timers) {
				if (!t.done && t.due <= until && (next == nullptr || t.due < next->due)) {
					next = &t;
				}
			}
			if (next == nullptr) {
				break;
			}
			now = std::max(now, next->due);
			next->done = true;
			auto fn = next->fn;
			fn();
		}
		now = until;
	}

	void respond(std::size_t index, const detail::response& r) {
		auto done = requests.at(index).done;
		done(r);
	}

	std::int64_t next_due() const {
		std::int64_t due = -1;
		for (const auto& t : timers) {
			if (!t.done && (due < 0 || t.due < due)) {
				due = t.due;
			}
		}
		return due;
	}
};

struct harness {
	fake_host* host;
	std::shared_ptr<detail::engine> engine;
	std::vector<std::pair<publisher, std::string>> disabled;
	std::vector<publisher_error> errors;
	std::vector<publisher> posted;
};

std::unique_ptr<harness> make(options opts = {}, std::optional<cluster_info> cluster = cluster_info{0, 1}) {
	auto h = std::make_unique<harness>();
	auto* raw = h.get();
	opts.on_disabled = [raw](publisher kind, int, const std::string& reason) { raw->disabled.emplace_back(kind, reason); };
	opts.on_error = [raw](const publisher_error& error) { raw->errors.push_back(error); };
	opts.on_posted = [raw](publisher kind, int) { raw->posted.push_back(kind); };
	auto host = std::make_unique<fake_host>();
	h->host = host.get();
	h->engine = std::make_shared<detail::engine>(std::move(host), std::move(opts), cluster, cluster ? cluster->id == 0 : true);
	return h;
}

options only(publisher kind) {
	options opts;
	opts.stats.enabled = kind == publisher::stats;
	opts.heartbeat.enabled = kind == publisher::heartbeat;
	opts.commands.enabled = kind == publisher::commands;
	return opts;
}

/** The result if it arrived synchronously, "pending" while a request is still in flight. */
post_result manual(detail::engine& engine, publisher kind) {
	auto out = std::make_shared<post_result>();
	out->skipped = "pending";
	engine.post(kind, [out](const post_result& r) { *out = r; });
	return *out;
}

void test_stats_publisher() {
	auto h = make(only(publisher::stats));
	h->engine->start();
	h->host->advance(4'999);
	CHECK(h->host->requests.empty());
	h->host->advance(1);
	CHECK(h->host->requests.size() == 1);
	CHECK(h->host->requests[0].path == "/bots/123/stats");
	CHECK(h->host->requests[0].body == json::parse(R"({"server_count":42,"shard_count":2})"));
	h->host->respond(0, reply(200, R"({"message":"Stats updated.","server_count":42})"));
	CHECK(h->posted.size() == 1);

	// Same number: nothing sent. A new number: posted on the next tick.
	h->host->advance(300'000);
	CHECK(h->host->requests.size() == 1);
	h->host->count = 43;
	h->host->advance(300'000);
	CHECK(h->host->requests.size() == 2);
	CHECK(h->host->requests[1].body["server_count"] == 43);
	h->host->respond(1, reply(200));

	// min_gap is a hard floor, manual posts included.
	h->host->count = 44;
	CHECK(manual(*h->engine, publisher::stats).skipped == "min-gap");
	CHECK(h->host->requests.size() == 2);

	// Guild churn debounces into one post.
	h->host->advance(60'000);
	CHECK(h->host->requests.size() == 3);
	h->host->respond(2, reply(200));
	h->host->count = 45;
	h->engine->guild_changed();
	h->engine->guild_changed();
	h->host->advance(19'999);
	CHECK(h->host->requests.size() == 3);
	h->host->advance(41'000);
	CHECK(h->host->requests.size() == 4);
}

void test_unauthorized_disables_everything() {
	auto h = make();
	h->engine->start();
	h->host->advance(0);
	CHECK(h->host->requests.size() == 1);
	CHECK(h->host->requests[0].path == "/bots/123/heartbeat");
	h->host->respond(0, reply(200, R"({"cluster":0,"status":"operational","shard_count":1,"next_beat_within_seconds":90})"));

	h->host->advance(5'000);
	CHECK(h->host->requests.size() == 2);
	CHECK(h->host->requests[1].path == "/bots/123/stats");
	h->host->respond(1, reply(401));

	CHECK(h->disabled.size() == 3);
	for (const auto& entry : h->disabled) {
		CHECK(entry.second == "unauthorized");
	}
	h->host->advance(3'600'000);
	CHECK(h->host->requests.size() == 2);
	CHECK(h->host->fetches.empty());

	// An explicit post re-arms the publisher, and its loop resumes.
	h->host->count = 50;
	CHECK(manual(*h->engine, publisher::stats).skipped == "pending");
	CHECK(h->host->requests.size() == 3);
	h->host->respond(2, reply(200));
	h->host->count = 51;
	h->host->advance(300'000);
	CHECK(h->host->requests.size() == 4);
}

void test_plan_suspends_heartbeat() {
	auto h = make(only(publisher::heartbeat));
	h->engine->start();
	h->host->advance(0);
	CHECK(h->host->requests.size() == 1);
	CHECK(h->host->requests[0].body ==
		json::parse(R"({"cluster":0,"cluster_count":1,"shards":[{"id":0,"status":"ready","latency_ms":40,"guilds":42}]})"));
	h->host->respond(0, reply(403));

	CHECK(h->disabled.empty());
	CHECK(h->errors.size() == 1);
	CHECK(h->errors[0].status == 403);
	CHECK(h->host->next_due() >= h->host->now + detail::plan_reprobe_ms);

	h->engine->beat_now();
	h->host->advance(detail::plan_reprobe_ms - 1'000);
	CHECK(h->host->requests.size() == 1);
	h->host->advance(2'000);
	CHECK(h->host->requests.size() == 2);
	h->host->respond(1, reply(200, R"({"next_beat_within_seconds":60})"));

	// Retuned to a third of the grace window.
	auto before = h->host->now;
	h->host->advance(19'999);
	CHECK(h->host->requests.size() == 2);
	h->host->advance(1);
	CHECK(h->host->requests.size() == 3);
	CHECK(h->host->now - before == 20'000);
}

void test_not_found_and_conflict_disable() {
	auto stats = make(only(publisher::stats));
	stats->engine->start();
	stats->host->advance(5'000);
	stats->host->respond(0, reply(404));
	CHECK(stats->disabled.size() == 1);
	CHECK(stats->disabled[0].first == publisher::stats);
	CHECK(stats->disabled[0].second == "not-found");
	stats->host->count = 99;
	stats->host->advance(3'600'000);
	CHECK(stats->host->requests.size() == 1);

	auto commands = make(only(publisher::commands));
	commands->engine->start();
	commands->host->advance(10'000);
	CHECK(commands->host->fetches.size() == 1);
	commands->host->fetches[0](std::string(R"([{"name":"ping","description":"Pong"}])"), "");
	CHECK(commands->host->requests.size() == 1);
	CHECK(commands->host->requests[0].path == "/bots/123/commands");
	CHECK(commands->host->requests[0].body == json::parse(R"({"commands":[{"name":"ping","description":"Pong"}]})"));
	commands->host->respond(0, reply(409));
	CHECK(commands->disabled.size() == 1);
	CHECK(commands->disabled[0].second == "conflict");
	CHECK(manual(*commands->engine, publisher::commands).skipped == "pending");
	CHECK(commands->host->fetches.size() == 2);
}

void test_throttle_is_shared_but_heartbeats_bypass() {
	options opts;
	opts.commands.enabled = false;
	auto h = make(opts);
	h->engine->start();
	h->host->advance(0);
	h->host->respond(0, reply(200));
	h->host->advance(5'000);
	CHECK(h->host->requests[1].path == "/bots/123/stats");
	h->host->respond(1, reply(429, "", "120"));

	// Commands share the suspension: dropped without a request.
	post_result dropped;
	h->engine->post(publisher::commands, [&](const post_result& r) { dropped = r; });
	h->host->fetches.at(0)(std::string(R"([{"name":"ping","description":"Pong"}])"), "");
	CHECK(dropped.skipped == "suspended");
	CHECK(h->host->requests.size() == 2);

	// Heartbeats do not.
	h->host->advance(35'000);
	CHECK(h->host->requests.size() == 3);
	CHECK(h->host->requests[2].path == "/bots/123/heartbeat");

	// Once the window passes, commands go out again.
	h->host->advance(120'000);
	h->engine->post(publisher::commands, {});
	h->host->fetches.at(1)(std::string(R"([{"name":"ping","description":"Pong"}])"), "");
	CHECK(h->host->requests.back().path == "/bots/123/commands");
}

void test_server_errors_are_retried() {
	auto h = make(only(publisher::stats));
	h->engine->start();
	h->host->advance(5'000);
	h->host->respond(0, reply(503));
	CHECK(h->errors.empty());
	h->host->advance(499);
	CHECK(h->host->requests.size() == 1);
	h->host->advance(1);
	CHECK(h->host->requests.size() == 2);
	h->host->respond(1, reply(0));
	h->host->advance(1'000);
	CHECK(h->host->requests.size() == 3);
	h->host->respond(2, reply(502));
	CHECK(h->errors.size() == 1);
	CHECK(h->errors[0].retryable);
	CHECK(h->errors[0].status == 502);
	h->host->advance(10'000);
	CHECK(h->host->requests.size() == 3);
	CHECK(h->disabled.empty());
}

void test_payload_errors_are_reported_once() {
	auto h = make(only(publisher::stats));
	h->engine->start();
	h->host->advance(5'000);
	h->host->respond(0, reply(422));
	h->host->count = 7;
	h->host->advance(300'000);
	h->host->respond(1, reply(422));
	CHECK(h->host->requests.size() == 2);
	CHECK(h->errors.size() == 1);
	CHECK(h->disabled.empty());
}

void test_unknown_cluster_count_never_beats() {
	auto h = make(options{}, std::nullopt);
	h->engine->start_heartbeat();
	h->engine->start();
	h->host->advance(600'000);
	for (const auto& r : h->host->requests) {
		CHECK(r.path != "/bots/123/heartbeat");
	}
	CHECK(h->disabled.size() == 1);
	CHECK(h->disabled[0].first == publisher::heartbeat);
	CHECK(h->disabled[0].second == "cluster-count-unknown");
	CHECK(manual(*h->engine, publisher::heartbeat).skipped == "cluster-count-unknown");
}

void test_commands_skip_empty_and_non_elected() {
	auto h = make(only(publisher::commands));
	h->engine->start();
	h->host->advance(10'000);
	h->host->fetches[0](std::string("[]"), "");
	CHECK(h->host->requests.empty());

	options opts = only(publisher::commands);
	opts.commands.post_empty = true;
	auto clears = make(opts);
	clears->engine->start();
	clears->host->advance(10'000);
	clears->host->fetches[0](std::string("[]"), "");
	CHECK(clears->host->requests.size() == 1);
	CHECK(clears->host->requests[0].body == json::parse(R"({"commands":[]})"));

	auto sibling = make(options{}, cluster_info{1, 2});
	sibling->engine->start();
	sibling->host->advance(60'000);
	for (const auto& r : sibling->host->requests) {
		CHECK(r.path == "/bots/123/heartbeat");
		CHECK(r.body["cluster"] == 1);
		CHECK(r.body["cluster_count"] == 2);
	}
	CHECK(sibling->host->fetches.empty());
	CHECK(manual(*sibling->engine, publisher::stats).skipped == "not-elected");
}

void test_missing_token_is_inert() {
	dpp::cluster bot("unused");
	autoposter poster(bot, "");
	post_result result;
	poster.post_stats([&](const post_result& r) { result = r; });
	CHECK(!result.ok);
	CHECK(result.skipped == "disabled");
	poster.post_heartbeat();
	poster.post_commands();
	poster.stop();
}

void test_misconfigured_cluster_refuses_heartbeats() {
	dpp::cluster bot("unused");
	options opts;
	opts.bot_id = "123";
	opts.cluster = cluster_info{3, 2};
	std::string reason;
	opts.on_disabled = [&](publisher, int, const std::string& r) { reason = r; };
	autoposter poster(bot, "token", opts);
	post_result result;
	poster.post_heartbeat([&](const post_result& r) { result = r; });
	CHECK(reason == "cluster-count-unknown");
	CHECK(result.skipped == "cluster-count-unknown");
}

}

int main() {
	test_webhooks();
	test_shaping();
	test_classify();
	test_stats_publisher();
	test_unauthorized_disables_everything();
	test_plan_suspends_heartbeat();
	test_not_found_and_conflict_disable();
	test_throttle_is_shared_but_heartbeats_bypass();
	test_server_errors_are_retried();
	test_payload_errors_are_reported_once();
	test_unknown_cluster_count_never_beats();
	test_commands_skip_empty_and_non_elected();
	test_missing_token_is_inert();
	test_misconfigured_cluster_refuses_heartbeats();

	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
