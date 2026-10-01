# Changelog

## 0.1.0 - Unreleased

- `thediscordlist::autoposter`: stats, per-cluster heartbeats and global
  commands for a `dpp::cluster`, with the shared 429 suspension, retries,
  plan reprobing and self-disabling on 401/404/409.
- `thediscordlist::verify_webhook` for `X-TDL-Signature`.
- CMake package (`find_package(thediscordlist-dpp)`), FetchContent support and a
  vcpkg overlay port.
