# Third-Party Notices

BigShark includes source code from the projects below. Their original
copyright and license notices remain in the vendored source files.

## OpenSpiel

- Project: <https://github.com/google-deepmind/open_spiel>
- License: Apache License 2.0
- Copyright: DeepMind Technologies Limited and OpenSpiel contributors
- Location: `engine/third_party/open_spiel/open_spiel/`

## Abseil

- Project: <https://github.com/abseil/abseil-cpp>
- License: Apache License 2.0
- Copyright: The Abseil Authors
- Location: `engine/third_party/open_spiel/open_spiel/abseil-cpp/`
- License text:
  `engine/third_party/open_spiel/open_spiel/abseil-cpp/LICENSE`

## JSON for Modern C++

- Project: <https://github.com/nlohmann/json>
- Version: 3.12.0
- License: MIT
- Copyright: Niels Lohmann and contributors
- Location: `engine/third_party/open_spiel/open_spiel/json/`

## yyjson

- Project: <https://github.com/ibireme/yyjson>
- Version: 0.10.0
- License: MIT
- Copyright: YaoYuan and contributors
- Location: `engine/third_party/yyjson/`

## SQLite

- Project: <https://www.sqlite.org/>
- Version: 3.50.0 (official amalgamation `sqlite-amalgamation-3500000`,
  published 2025-05-29)
- License: Public Domain ("SQLite Is Public Domain")
- Location: `engine/third_party/sqlite/` (`sqlite3.c`, `sqlite3.h`,
  `sqlite3ext.h`, unmodified)
- Use: compiled privately into the `bigshark_artifacts` static library with
  `SQLITE_THREADSAFE=1` and `SQLITE_OMIT_LOAD_EXTENSION=1`; the system or
  Homebrew SQLite is never linked. The public-domain blessing is recorded in
  `engine/third_party/sqlite/LICENSE`.
- Verification: recorded in the RFC 0005 Stage 5 evidence block of
  `docs/plans/0004-0006-implementation.md`.

## OpenSSL 3 (Crypto)

- Project: <https://www.openssl.org/>
- Version used in verification: OpenSSL 3.6.3
  (`/opt/homebrew/opt/openssl@3`, Homebrew `openssl@3` 3.6.3, Apple
  arm64). Linux builds link the distribution OpenSSL 3.
- License: Apache License 2.0 (OpenSSL 3.x)
- Location: not vendored; linked dynamically. CMake prefers the discovered
  `OpenSSL::Crypto` target and falls back to the Homebrew `openssl@3`
  keg-only prefix. Only `libcrypto` is used, linked PRIVATELY into
  `bigshark_artifacts`; the system certificate/TLS stack and `libssl` are not
  pulled in.
- License text: `engine/third_party/openssl/LICENSE` (verbatim Apache 2.0
  text copied from the linked Homebrew installation).
