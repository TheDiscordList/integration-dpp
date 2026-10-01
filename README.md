# thediscordlist-dpp

Publish your [D++](https://dpp.dev) bot's server count, cluster heartbeats and
application commands to [TheDiscordList](https://thediscordlist.com)
automatically, and verify its inbound webhooks.

C++17, no dependencies beyond D++ itself: HTTP goes through
`dpp::cluster::request`, scheduling through D++ timers, JSON through the
nlohmann::json that D++ bundles, and webhook signatures through the OpenSSL
that D++ already links.

```sh
vcpkg install thediscordlist-dpp
```

Until the port is merged into the vcpkg registry, use the overlay in this
repository (`--overlay-ports=ports`), CMake's `FetchContent`, or an installed
copy (see [Installing](#installing)).

## Quickstart

```cpp
#include <dpp/dpp.h>
#include <thediscordlist/dpp.h>

#include <cstdlib>

int main() {
    dpp::cluster bot(std::getenv("DISCORD_TOKEN"));
    bot.on_log(dpp::utility::cout_logger());

    const char* tdl_token = std::getenv("TDL_TOKEN");
    thediscordlist::autoposter poster(bot, tdl_token ? tdl_token : "");

    bot.start(dpp::st_wait);
}
```

```cmake
find_package(thediscordlist-dpp CONFIG REQUIRED)
target_link_libraries(bot PRIVATE thediscordlist::dpp)
```

That is the whole integration, and it is also the recommended production setup.
It starts three publishers:

| Publisher | What it sends | Cadence |
| --- | --- | --- |
| Stats | `server_count`, `shard_count` | 5s after the first shard is ready, then every 5 min, debounced on guild join/leave |
| Heartbeat | per-shard status, latency and guild counts | every ~30s, plus 1s after any shard becomes ready or resumes |
| Commands | your global slash commands | once, ~10s after ready |

Your **stats token** is on the listing's manage page, under the API tab. It is
scoped to one listing, so it is the only credential this package needs.

The `autoposter` can be constructed before or after `bot.start()`. With an
empty token it logs a warning and stays inert — a forgotten environment
variable never takes down a deploy. Keep it alive for as long as it should
post, and destroy it before the cluster (declaring it after the cluster does
that for you).

Diagnostics go through `bot.log()`, prefixed `[thediscordlist]`, so they show
up wherever your `on_log` handler sends D++'s own logs.

## Sharding and clustering

D++ is told its layout up front — `dpp::cluster(token, intents, shards,
cluster_id, maxclusters)` — so nothing has to be guessed:

| Setup | `server_count` comes from | Stats and commands posted by | Heartbeats |
| --- | --- | --- | --- |
| One process, any number of shards | `dpp::get_guild_count()` | this process | this process, cluster 0 of 1 |
| `maxclusters > 1`, one process per cluster | your `options.server_count` | cluster 0 | every cluster, for itself |

Each heartbeat carries `cluster_id` and `maxclusters` as `cluster` and
`cluster_count`, and lists the shards this cluster owns (`id % maxclusters ==
cluster_id`). Shards D++ has not created yet during a staggered start report
`connecting`; a dropped shard D++ is resuming reports `resuming`.

Every cluster heartbeats for **itself** on purpose. A manager reporting "all
fine" for a cluster whose host has died is exactly the failure the status page
exists to catch.

D++ has no cross-process channel, and its guild cache only holds this process's
guilds, so a clustered bot must supply the bot-wide total for cluster 0 to post
stats:

```cpp
thediscordlist::options options;
options.server_count = []() -> std::optional<std::uint64_t> {
    return my_redis.sum_guild_counts();  // std::nullopt skips this round
};
```

Without it, cluster 0 logs a warning and posts no stats rather than a partial
number.

> **If the cluster count is invalid, heartbeats do not start.** The API
> hard-deletes every cluster row whose id is `>= cluster_count` in the request,
> so a process reporting the wrong count would erase its siblings and their
> monitoring history. A count outside 1–1024, or an id not below it, disables
> the heartbeat publisher with reason `cluster-count-unknown`. Fix it with
> `options.cluster = {id, count}`; losing a status page beats losing data.

## Options

```cpp
thediscordlist::options options;
options.bot_id = "161660517914509312";            // default: bot.me.id once ready
options.base_url = "https://thediscordlist.com/api";

options.stats.enabled = true;
options.stats.interval = std::chrono::minutes(5);
options.stats.debounce = std::chrono::seconds(20);
options.stats.min_gap = std::chrono::seconds(60);

options.heartbeat.enabled = true;
options.heartbeat.interval = std::chrono::seconds(30); // 15–60s, retuned from the API

options.commands.enabled = true;
options.commands.post_empty = false;

options.cluster = thediscordlist::cluster_info{0, 1};  // default: bot.cluster_id / bot.maxclusters
options.server_count = [] { return std::optional<std::uint64_t>(1234); };

thediscordlist::autoposter poster(bot, token, options);
```

`min_gap` is a hard floor between two stats POSTs. A join storm, a mass
reconnect and a manual `post_stats()` all route through it, so the ceiling is
structurally one request per minute. An unchanged count is not re-sent for 30
minutes: every stats post re-indexes the listing and fires a webhook.

`commands.post_empty` is `false` because posting an empty list **clears** the
commands on your listing. Turn it on only if that is what you mean.

`enabled = false` stops a publisher from running on its own; a manual post
still works.

`bot_id` is your Discord **application** id. The default, `bot.me.id`, is the
bot user's id, which equals the application id for nearly every bot (D++ reads
application commands with it too). Set it by hand if yours differs, or to start
heartbeating before the first `READY`.

## Events and manual posting

```cpp
options.on_posted = [](thediscordlist::publisher type, int status) {};
options.on_error = [](const thediscordlist::publisher_error& error) {};  // type, status, retryable, message
options.on_disabled = [](thediscordlist::publisher type, int status, const std::string& reason) {};

poster.post_stats([](const thediscordlist::post_result& result) {
    // result.ok, result.status (0 = no response), result.skipped ("unchanged", "min-gap", ...)
});
poster.post_commands();  // after re-registering commands in-process
poster.post_heartbeat();

poster.stop();
```

Callbacks run on D++ threads (an immediate skip answers on the calling
thread), outside the poster's lock. Nothing in this
package throws into your bot: timer and HTTP callbacks are wrapped, a callback
of yours that throws is logged, and manual posts report through `post_result`.

An explicit `post_*()` also re-arms a publisher that disabled itself, so you can
retry after fixing a token or a listing id without restarting.

To block on a manual post from your own thread (never from a D++ event handler):

```cpp
std::promise<thediscordlist::post_result> done;
poster.post_stats([&](const auto& result) { done.set_value(result); });
auto result = done.get_future().get();
```

## Webhooks

`thediscordlist/webhooks.h` needs only nlohmann::json and OpenSSL — not a
running cluster — so it also works in a service that never connects to Discord.

```cpp
#include <thediscordlist/webhooks.h>

try {
    auto event = thediscordlist::verify_webhook(raw_body, signature_header, secret);
    // event.id, event.event, event.created_at, event.listing.{id,type,name,url}, event.data
} catch (const thediscordlist::webhook_error& error) {
    // error.code(): missing_signature, malformed_signature, stale_timestamp,
    // invalid_signature or malformed_body; to_string(error.code()) gives "INVALID_SIGNATURE" etc.
}
```

`raw_body` must be the exact bytes that arrived. TheDiscordList signs the JSON
string it sends; re-serialising a parsed body changes escaping and key order and
can never verify. The timestamp window is 300 seconds either side by default
(`tolerance` is the fourth argument).

### With D++'s HTTP server

`dpp::http_server` needs D++ 10.1 or later. On 10.0, call `verify_webhook` from
whatever HTTP server you already run.

```cpp
dpp::http_server webhooks(&bot, "0.0.0.0", 8080, [secret](dpp::http_server_request* request) {
    try {
        auto event = thediscordlist::verify_webhook(request->get_request_body(),
            request->get_header(thediscordlist::webhook_signature_header), secret);
        if (event.event == "listing.upvoted") {
            // event.data["user"]["username"], event.data["total_upvotes"], ...
        }
        request->set_status(204);
    } catch (const thediscordlist::webhook_error&) {
        request->set_status(400);
    }
});
```

`dpp::http_server` is plain HTTP unless you pass a key and certificate; put it
behind your TLS-terminating proxy and register the public `https://` URL. The
handler runs on a D++ worker thread and answers when it returns, so hand slow
work to a queue rather than doing it inline. [`examples/bot.cpp`](examples/bot.cpp) is a complete bot with
this wired up.

### Events

| Event | Fires for | `data` |
| --- | --- | --- |
| `listing.bumped` | servers, bots | `points`, `bumped_at`, `next_bump_at`, `user` |
| `listing.upvoted` | servers, bots | `voted_at`, `total_upvotes`, `user` |
| `server.joined` | servers | `joined_at`, `user` |
| `bot.added` | bots | `added_at`, `user` |
| `bot.stats_reported` | bots | `guild_count`, `shard_count`, `reported_at` |
| `webhook.ping` | both | `message` |

`user` is `null` or `{id, username, avatar}`, and `id` can itself be `null`.
Dates are ISO 8601 with an offset. Unknown event names parse like any other, so
new events never break an existing endpoint.

`bot.added` and `server.joined` are **click** events, dispatched on the install
hand-off; Discord never confirms completion. `bot.stats_reported` is **not** an
echo of your own post — it also fires when the owner edits the listing by hand.
`created_at` is enqueue time, not send time; never use it for freshness.

### Rules for a healthy endpoint

- **Always answer 2xx**, including for `webhook.ping` and for event names you do
  not recognise. The sender disables an endpoint after 10 consecutive failed
  deliveries. Answer 400 only when verification fails.
- A disabled endpoint is probed with a `webhook.ping` every 6 hours for 7 days,
  and **a 2xx to that probe re-enables it**.
- **Register an exact `https://` URL with no redirect.** The sender treats 3xx
  as a failure.
- **Deduplicate on `X-TDL-Delivery`** (equal to `event.id`). It is stable across
  all five retries of one delivery. A manual "redeliver" from the web UI mints a
  new id on purpose.
- **Rotating the secret has no grace window.** Deliveries already queued under
  the old secret become unverifiable.

## Plans, limits and known behaviour

- **Heartbeats need Gold or higher.** On a `403` the heartbeat publisher
  suspends and reprobes every 30 minutes rather than disabling: the plan is
  checked per request, so a bot can gain it mid-run.
- **Rate limits are shared.** Stats and commands share 60 requests a minute per
  client IP; heartbeats have 120. On a `429` the poster honours `Retry-After`
  and `X-RateLimit-Reset` with one client-wide suspension that stats and
  commands share, while heartbeats keep going — a stale number beats a false
  "down" on your status page. Other bots behind the same egress IP count
  against you.
- **`404` never means "bad token".** It means the id is unknown or belongs to a
  server listing; the publisher disables itself with reason `not-found`. A
  `401` is the bad-token case and disables every publisher. A `409` on commands
  means the listing is not set up for slash commands.
- `5xx`, timeouts and network errors are retried up to 3 times with backoff,
  then wait for the next tick. Requests time out after 10 seconds.
- D++ timers tick once a second, so every delay here is whole seconds.
- The server count is D++'s guild cache. Without `dpp::i_guilds` the cache never
  changes (the poster warns once), and a cache policy with `guild_policy =
  dpp::cp_none` reports 0.

## Installing

**vcpkg.** `vcpkg install thediscordlist-dpp`, or `"thediscordlist-dpp"` in your
`vcpkg.json`. Before the registry has it:
`vcpkg install thediscordlist-dpp --overlay-ports=path/to/this/repo/ports`.

**FetchContent.** Builds this repository as part of yours; D++ is found with
`find_package(dpp)`, or reused if your build already defines the `dpp` target.

```cmake
include(FetchContent)
FetchContent_Declare(thediscordlist-dpp
    GIT_REPOSITORY https://github.com/TheDiscordList/integration-dpp.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(thediscordlist-dpp)

target_link_libraries(bot PRIVATE thediscordlist::dpp)
```

**From source.**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix /usr/local
```

then `find_package(thediscordlist-dpp CONFIG REQUIRED)`.

**Conan** is not published yet. D++ ships a `conanfile.py`, so a recipe
wrapping this CMake project is the likely next channel.

## Requirements

- A C++17 compiler (the D++ 10.1 `.deb` raises its consumers to C++20)
- CMake 3.16+
- D++ 10.x — CI builds against 10.0.35 and 10.1.6
- OpenSSL, which D++ already depends on

## Publishing (maintainers)

Releases are GitHub Releases cut from a tag; distribution is the vcpkg port.
Nothing here needs a registry token.

One-time setup:

1. Create `TheDiscordList/integration-dpp` and push `main`; the release
   workflow publishes with the built-in `GITHUB_TOKEN` (`contents: write`).
2. Fork `microsoft/vcpkg` under an account that will open the port PR.

Every release:

1. Bump the version in `CMakeLists.txt` (`project(... VERSION)`),
   `include/thediscordlist/dpp.h` (`version`),
   `ports/thediscordlist-dpp/vcpkg.json` and `CHANGELOG.md`. The release
   workflow refuses a tag that disagrees with any of them.
2. `git tag v0.1.0 && git push origin v0.1.0`. The workflow builds, tests,
   attaches a source tarball and prints the vcpkg `SHA512` of the tag archive in
   the release notes.
3. In the vcpkg fork, copy `ports/thediscordlist-dpp` to `ports/` and replace
   `SHA512 0` in `portfile.cmake` with the hash from the release notes.
4. `./vcpkg install thediscordlist-dpp` to check it builds, then
   `./vcpkg format-manifest ports/thediscordlist-dpp/vcpkg.json` and
   `./vcpkg x-add-version thediscordlist-dpp`.
5. Commit `ports/thediscordlist-dpp` and `versions/`, and open a PR against
   `microsoft/vcpkg` titled `[thediscordlist-dpp] new port` (later:
   `[thediscordlist-dpp] update to X.Y.Z`).

## License

MIT
