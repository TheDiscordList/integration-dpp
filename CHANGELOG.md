# Changelog

## 0.2.1 - 2026-11-03

- A one-shot timer no longer uses its own callback after stopping it, which is a
  use-after-free on D++ 10.0, where `stop_timer()` deletes the running callback.
- Heartbeats omit a shard's `guilds` until the shard is ready and D++ has cached
  the guilds `READY` listed, and stats wait for every shard, so a restart cannot
  publish a 0 or a partial count.
- The bot id defaults to the application id fetched from Discord instead of
  `bot.me.id`, which gave older bots a `404` that disabled the publisher.
  Commands are fetched with it too, rather than through `global_commands_get()`.
- Commands are retried every 5 minutes after a `5xx`, a `429` or a failed fetch
  from Discord, until they post or get a terminal `4xx`.
- A `429` on a heartbeat no longer suspends stats and commands, whose limit is
  separate.
- `stop()` also ends a pending retry, which now reports `stopped`.
- The release workflow pins its actions to commit SHAs and refuses a tag that is
  not on `main`.

## 0.2.0 - 2026-10-18

- `verify_webhook` fails closed on an empty secret with the new
  `webhook_error_code::missing_secret`, instead of computing an HMAC with an
  empty key.
- `listing.bump_ready` in the documented webhook events.
- `408` and `425` are retried like `5xx`; every other `4xx` is reported and not
  retried. A `429` is retried once its wait is over if that comes before the
  next tick, and its suspension is capped at an hour.
- An out-of-band heartbeat when a shard becomes ready or resumes waits until at
  least 10 seconds after the previous beat, instead of 1 second.
- `options.stats.min_gap` is never below 60 seconds.
- Suspensions and gaps run on `std::chrono::steady_clock`.
- The stats token is trimmed and a pasted `Bearer ` prefix is stripped.
- The version test covers `ports/thediscordlist-dpp/vcpkg.json` too.

## 0.1.0 - 2026-10-16

- `thediscordlist::autoposter`: stats, per-cluster heartbeats and global
  commands for a `dpp::cluster`, with the shared 429 suspension, retries,
  plan reprobing and self-disabling on 401/404/409.
- `thediscordlist::verify_webhook` for `X-TDL-Signature`.
- CMake package (`find_package(thediscordlist-dpp)`), FetchContent support and a
  vcpkg overlay port.
