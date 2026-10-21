# Storage reliability release

## Context and purpose

Reviewed 2026-10-21. Baseline: `a1cdda7`, clean `main`, existing origin left unchanged.
No repository AGENTS.md or applicable parent instructions were found. Existing
strengths: bounded Modbus operations, RAII transport ownership, transactional
retention, durable QoS 1 outbox, Linux process/protocol tests, sanitizer CI.
Gaps: storage tests are coupled to Linux transport headers; failed-commit identity
and partial-prune rollback lack direct regression coverage. Existing build files
are Linux ELF artifacts, not evidence of a run on this host.

Users are developers/operators collecting meter readings in a local Linux lab.
Their friction is diagnosing storage failures without losing pending telemetry or
mistaking an uncommitted reading for a saved reading. The improvement belongs in
the existing Store, not a replacement gateway or new dashboard.

## Release requirements and acceptance

Required:
- Preserve SQLite schema, REST/MQTT payloads, retention policy and Linux runtime.
- Separate the already independent reading/storage code from POSIX transport so
  the actual Store can be verified with real SQLite on Windows as well as Linux.
- Failed append leaves the caller's identity and persisted history unchanged;
  retry works, pending entries remain ordered, and acknowledged history alone is
  eligible for eviction.
- Cover commit failure, insert failure after pruning, reopen/replay, mixed
  acknowledged/pending retention and invalid Store capacity with direct tests.
- Record genuine defects and one intentional rollback fault in an isolated
  worktree, preserving red/green evidence and integrating useful coverage.
- Provide reproducible build commands, journal, walkthrough and interview notes.

Success: native storage checks pass; existing Linux checks are rerun if a Linux
runtime becomes available. Otherwise explicitly report those checks blocked.
No production throughput or power-loss guarantees will be inferred from unit tests.

Follow-ups: configurable register maps, TLS/authentication, metrics and storage
latency benchmarks. Outside scope: Windows gateway port, cloud service, multiple
meters/databases, UI, hardware certification, system-wide installs, publication.

## Research completed before implementation

All references reviewed 2026-10-21; documentation/release pages only, not runtime
product evaluations. No reference code or assets are copied.

| Reference and inspected URLs | Users/problem and useful decisions | Relevance evidence | Adopt / differ / unknowns |
|---|---|---|---|
| Telegraf [Modbus docs](https://docs.influxdata.com/telegraf/v1/input-plugins/modbus/), [releases](https://github.com/influxdata/telegraf/releases) | Operators ingesting PLC/meter registers; explicit byte order, scale, timeout and per-device configuration | Release page includes v1.38.2 (2026-03-30) and v1.38.3 artifacts | Adopt explicit protocol contracts and bounded failures; retain a much smaller fixed register map and embedded history. No claim of plugin/feature parity; output durability not evaluated. MIT source license would require preservation if copied. |
| SQLite [transactions](https://www.sqlite.org/lang_transaction.html), [release history](https://www.sqlite.org/changes.html) | Embedded applications need atomic persistence; COMMIT can fail and a busy transaction may stay active | Release history lists 3.54.1 dated 2026-10-16; 3.51.3 fixes WAL-reset corruption | Test actual SQLite failures, not a mock database. Publish reading ID only after commit. Host filesystem/power-loss behavior remains unknown. SQLite is public domain; downloaded test dependency remains outside Git. |
| Mosquitto [client API](https://mosquitto.org/api/files/mosquitto-h.html), [2.1.2 release](https://mosquitto.org/blog/2026/02/version-2-1-2-released/) | MQTT applications need connection loops and delivery callbacks | Dated 2026-02 bugfix release includes persistence configuration guard | Preserve PUBACK-driven acknowledgement and durable pending rows. Broker persistence is not subscriber processing or exactly-once delivery. No client architecture inferred beyond documented API; no code copied (EPL/EDL source terms apply if later reused). |

Comparison: Telegraf offers broad configurable ingestion; MeterBridge deliberately
supports one register map. SQLite provides database atomicity, not caller-state
atomicity. Mosquitto provides transport acknowledgement, not an application-owned
history queue. The durable need is inspectable telemetry during failure, not
popularity or equivalent maturity to these projects.

## Design and decisions

Keep one mutex-owned SQLite connection and the existing transaction per append.
Extract reading/Store declarations and implementations into small independent
modules, retaining core.hpp as the gateway's aggregation header. No repository
or service layer: it adds no value for one database. A storage-only CMake mode
should use an explicit SQLite dependency and avoid fetching HTTP/Modbus/MQTT.
Default Linux configuration must remain the gateway build.

Prefer real SQLite commit hooks/triggers in disposable tests to production fault
switches. Caller state must change only after durable commit; rollback handles
partial retention and write failure. Native tests are narrower than Linux process
integration and cannot replace it.

### Module and state ownership

```text
Linux-only                            Portable
main.cpp -> core.cpp (Meter/Args) --> reading.cpp (decode/JSON)
   |                                     |
   +-- HTTP handlers ----------------> store.cpp (SQLite)
   +-- poller ----------------------->   |
   +-- mqtt.cpp (Publisher) ---------->   +-- readings + sent flag
```

`reading.hpp` has no platform/library dependency. `store.hpp` owns SQLite and its
mutex; `core.hpp` aggregates it for compatibility with existing gateway callers.
There is no schema migration: existing DBs and IDs remain valid. `max_rows` must
be positive at Store construction. Append retains caller state on failure and
assigns the committed ID on success; identity uniqueness is per database.

The main thread owns Store longer than Publisher/poller/HTTP users. Modbus has one
polling owner; SQLite access is serialized; MQTT callbacks synchronize packet
acknowledgement using Publisher's mutex. Store finalizes prepared statements via
RAII and closes its connection in its destructor. Test-only auto-extension commit
hooks are scoped and unregistered; file fixtures remove only their unique temp
directory. File, broker and database errors remain visible through existing health
and retry paths. This release does not change shutdown or network retry behavior.

Storage-only CMake links just SQLite and Threads. Normal Linux builds link the
same storage library into meter-core, preserving application behavior. The split
adds a small library boundary to remove unnecessary transport coupling; it does
not add an interface, repository abstraction or runtime plugin system.

## Journal

- Initial inspection: clean main at a1cdda7, four prior commits. Read all source,
  tests, scripts, CI, README, CMake and existing documentation. No user edits.
- Environment: Windows Git Bash, MinGW GCC 16.1.0, CMake 4.4.4. WSL reports no
  installed distributions; Docker/Podman unavailable. Existing ELF tests cannot
  run here. Do not install a distro or claim historical Linux results as new runs.
- Research above finished before storage changes. Selected a bounded storage
  reliability release with portable direct verification; Linux runtime gate is
  explicitly unresolved.
- `e82f808`: extracted Reading/Store, added storage-only CMake and baseline
  checks. Real SQLite 3.54.1 from a checksum-verified public-domain amalgamation
  built with MinGW. Baseline passed; no gateway port attempted.
- `1a53612`: reproduced MB-001 with a SQLite commit hook, then repaired caller-ID
  publication. Database rollback already worked; caller state did not. Red and
  green logs retained.
- Controlled MB-002 in a separate worktree: verified test, omitted ROLLBACK,
  distinguished transaction-local from committed state using two connections,
  restored cleanup. `3062324` integrates coverage, not a fictitious main fix.
- `4691e65`: MB-003 rejects nonpositive Store capacity before creating files;
  tests cover 0 and -1. Added observer coverage and investigation provenance.
- Expanded direct tests cover insert rejection after eviction, repeated ACK,
  two reopens with signed/unsigned numeric boundaries, and real writer contention
  with successful retry. Seven native scenarios pass (1.10 s); no benchmark claim.
- Final fresh build of `1244d72`: 7 passed, 0 failed/skipped in 1.09 s. Five direct
  storage demonstrations also passed; outputs and environment recorded in
  `docs/evidence/`. Linux app launch, TCP/RTU/MQTT integration and sanitizers
  remain blocked on this host. Walkthrough distinguishes expected Linux behavior
  from performed native checks; interview guide references actual tests/commits.
- Publication review: unchanged MIT license and existing attribution; no copied
  reference code/assets. Downloaded SQLite and compiled outputs are ignored.
  Targeted tracked-file credential-pattern review found no matches; it is not a
  comprehensive secret audit. Origin is unchanged and nothing has been pushed.
