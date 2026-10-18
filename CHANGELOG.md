# Changelog

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
