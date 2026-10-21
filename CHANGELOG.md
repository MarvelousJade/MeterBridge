# Changelog

## 0.1.1 — 2026-10-21 (local, not published)

### Fixed
- Failed SQLite COMMIT no longer changes the caller's Reading ID (`1a53612`).
- Nonpositive Store capacity fails before creating a database (`4691e65`).

### Changed
- Reading and SQLite Store are separate, portable modules. Default application
  remains Linux-only; optional `METERBRIDGE_STORAGE_ONLY` avoids transport and
  HTTP dependencies (`e82f808`). Schema and REST/MQTT payloads are unchanged.

### Verification and documentation
- Added retention rollback coverage after reducing capacity (`3062324`), plus
  independent-connection observation (`4691e65`). The existing rollback was
  correct; the deliberate fault used to validate coverage stayed isolated.
- Seven storage scenarios cover encoding/outbox baseline, commit failure,
  partial pruning, invalid capacity, insert failure, reopen/replay and writer
  contention (`1244d72`). Tests use actual SQLite and disposable databases.
- Added current references, requirements, architecture decisions, investigation
  logs, reproducible verification, walkthrough and evidence-based interview guide.
- Native storage checks pass; Linux gateway/integration/sanitizer rerun is blocked
  by the absence of a Linux runtime on this host. Hardware and hosted CI remain
  unverified; see `docs/verification.md`.

## 0.1.0 — existing baseline

Modbus TCP/RTU polling, SQLite history and retention, local REST health/readings,
durable MQTT outbox, simulator, integration tests and Linux deployment scripts.
See existing history through `a1cdda7` and README's historical verification notes.
