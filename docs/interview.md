# Interview discussion guide

Use these as explanations of repository evidence, not as unverified claims about
your personal experience. Adapt first-person language only after you have run
and reviewed the work yourself. Deliberate faults must be described as deliberate.

## 30-second explanation

MeterBridge is a C++20 Linux gateway that polls an energy meter over Modbus,
keeps SQLite history, exposes local HTTP health/readings, and forwards telemetry
through a durable MQTT outbox. This release hardens the persistence boundary:
a rejected commit must not publish a saved identity, and failed retention must
leave pending data intact. Portable tests exercise the real database code, while
Linux protocol tests remain a separate verification layer.

## Two-minute technical walkthrough

- **0:00–0:25, user/problem:** operators need readable local meter history even
  when the meter or broker is unavailable. A fixed simulator makes the workflow
  reproducible without physical hardware.
- **0:25–0:55, data path:** one poller owns Modbus; Reading handles decoding and
  JSON; Store owns one mutex-serialized SQLite connection. Append prunes only
  acknowledged history and inserts within BEGIN IMMEDIATE/COMMIT. REST reads the
  store; Publisher fetches oldest pending and marks it sent only after PUBACK.
- **0:55–1:25, design:** retain this compact architecture rather than introduce a
  queue service. Separate storage from POSIX transport to run genuine SQLite
  tests on Windows without pretending the whole gateway is portable. Schema and
  wire contracts are unchanged.
- **1:25–1:50, defect:** commit rejection rolled SQL back but had already assigned
  an ID to the caller. The regression proved old history was intact while the
  object held ID 2 instead of 777. Stage ID locally, commit, then publish it.
- **1:50–2:00, evidence/limits:** seven native storage cases pass; Linux runtime,
  sanitizers and hardware must still be verified on the current source. No
  throughput or exactly-once claim.

## Five-minute demonstration

Prerequisite: use a Linux host and finish [walkthrough](walkthrough.md) once before
an interview. This session could not run the gateway on Windows.

| Time | Action | Observable point |
|---|---|---|
| 0:00–1:00 | Launch `sh scripts/demo.sh` | Included meter gives 230 V / 5 A / 1150 W; health becomes ok |
| 1:00–2:00 | Query readings twice and invalid `limit=0` | Newest-first JSON, read-only access, explicit 400 |
| 2:00–3:00 | Separate-terminal setup: stop/restart simulator | Health degrades, stored data remain, polling recovers |
| 3:00–4:00 | Run storage `commit_failure` and `partial_prune` | Failed write preserves identity and history |
| 4:00–5:00 | Show diff `git show 1a53612` and red/green logs | Root cause, small fix, regression, honest boundaries |

**Backup on this host:** use `sh scripts/check-storage.sh` with the documented
SQLite flags, show all seven tests, run two targeted demonstrations, then trace
`src/store.cpp` and read the historical failure logs. This is a storage demo,
not a substitute claim of a live meter, broker or UI demo. Keep local build and
SQLite archive cached in ignored folders if interview network access is uncertain.

## Debugging stories and evidence

### MB-001: SQL atomicity is not object atomicity

- Found by inspecting assignment order, triggered using SQLite's commit hook.
  Existing failure path, **not an injected implementation bug**.
- Impact: append throws yet exposes an unsaved ID. Current poller drops the
  reading, so do not exaggerate it as observed production telemetry loss.
- Reproduction and hypotheses: fill a one-row store with acknowledged history,
  deny commit on a replacement append. Was history lost, or was caller state
  published too early? History and pending-count assertions pass; ID is wrong.
- Root cause/design gap: original transaction reasoning covered SQL state only.
  `r.id = last_insert_rowid` preceded COMMIT.
- Change/correctness: assignment follows successful COMMIT under the same mutex;
  a thrown COMMIT cannot reach it. Retry on the same connection is checked.
- Prevention: `storage_commit_failure`, `tests/storage.cpp`; fix `1a53612`;
  [full investigation and logs](investigations.md#mb-001--failed-commit-publishes-an-uncommitted-identity).
- Principle: publish externally observable state after the fallible operation.

### MB-002: rollback is resource cleanup as well as data protection

- **Intentionally introduced** in a disposable worktree after a passing baseline.
  Removing ROLLBACK simulated an exception-cleanup regression.
- Symptom: rejected append leaves one row missing from the writer's view. An
  independent connection still sees three committed rows, rejecting durable
  deletion as the explanation; the active transaction owns a partial prune.
- Root cause: DELETE succeeded before the capacity check threw. Without cleanup,
  the transaction/lock survived, blocking the next BEGIN and distorting history.
- Repair: restore existing cleanup, verify original history and a successful
  retry after acknowledging another sample. Main already had correct code.
- Coverage integrated in `3062324` and observer assertions in `4691e65`;
  isolated fault `0abaaf8`, repair `834c572`. Explain this as regression coverage,
  not a nonexistent main implementation fix. See [MB-002](investigations.md).
- Principle: inspect state from more than one ownership boundary; an uncommitted
  write can affect the application without being durable.

### MB-003: validate at the owning boundary

Direct Store construction accepted zero/negative capacity even though CLI guarded
it. `storage_invalid_capacity` failed before the fix; `4691e65` adds validation
before sqlite3_open. The test checks both exception and no created file. This is
a library invariant gap, not an observed CLI failure. See [MB-003](investigations.md).

## Architecture and tradeoff questions

- **Why SQLite instead of an external queue?** One gateway/database and bounded
  local history fit one embedded transactional dependency. Shared mutex limits
  throughput and can make REST wait on a busy write; no benchmark yet justifies
  a pool, WAL reader connections or a new service.
- **Why no exactly-once claim?** A crash after broker PUBACK but before local ACK
  can replay the row. QoS 1 allows duplicates; IDs identify rows within a DB only.
  Broker receipt also does not prove downstream application processing.
- **Why reject when the outbox is full?** Deleting pending telemetry would violate
  the durable-delivery contract. Finite capacity means new measurements can be
  lost during long outages; make that visible via health and error counters.
- **Why storage-only rather than a Windows port?** It isolates testable core
  logic without replacing supported Linux serial/network integration. Portability
  of tests is useful, but must not be sold as gateway compatibility.
- **Why no generic transaction abstraction?** There is one append transaction.
  Explicit rollback is inspectable; add RAII only if more transactions make that
  discipline hard to maintain. Cleanup errors currently remain a limitation.
- **What does a commit hook prove?** The API's failure contract and C++ state
  ordering, not filesystem reliability under power interruption or all I/O errors.
- **What does reopen prove?** Real file serialization and ACK persistence across
  connection lifetimes, not SIGKILL survival. Existing Linux integration tests
  include SIGKILL replay but were not rerun on this host.

## Testing, performance and next work

Testing layers: pure decode boundaries; real SQLite in-memory and file-backed
regressions; external competing writer; existing Python subprocess/protocol tests;
Linux sanitizer CI configuration. Assertions target identity, row ordering,
retention and recovery rather than SQL implementation text. Disposable fault data
never touch a user database. Seven is the count for one full native run, not the
sum of historical runs. See [verification ledger](verification.md).

Measured native suite duration is about one second (see exact run logs). That is
not a throughput benchmark. No RAM, latency-percentile or ARM performance claims.
Next priorities: run current Linux integration/sanitizers, validate physical RS485,
then measure disk commit/REST contention at realistic capacity. Consider register
map configuration and TLS only with concrete deployment needs. Avoid adding a
GUI or cloud service solely to broaden the feature list.
