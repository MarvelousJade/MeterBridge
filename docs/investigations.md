# Storage investigations

## MB-001 — Failed commit publishes an uncommitted identity

- Origin: genuine pre-existing defect, `a1cdda7:src/core.cpp`, moved unchanged to
  `e82f808:src/store.cpp`. Found by code investigation, then reproduced on the
  extracted production Store with a real SQLite connection.
- Impact: append throws but mutates the caller's Reading ID to an unsaved row.
  The current poller discards failed readings, limiting present user impact;
  library callers/retry logic cannot trust the object to reflect persistence.
- Invariant: database retention and caller-visible identity both change only
  after successful commit.
- Initial hypotheses: SQLite rollback might fail to restore a pruned row, or
  caller mutation might occur before SQLite's commit boundary. The test fills a
  capacity-one store with acknowledged history, then denies COMMIT through
  SQLite's commit hook. This simulates commit rejection, not physical disk loss.
- Observation: append throws `constraint failed`; history is restored and pending
  count is zero, rejecting the failed-database-rollback hypothesis. Caller ID is
  **2**, not the original **777**. See [red log](evidence/commit-failure-red.txt).
- Root cause: `sqlite3_last_insert_rowid` was assigned directly to `r.id` before
  COMMIT. SQL rollback cannot restore C++ object state.
- Repair: stage the ID in a local, COMMIT, then assign to the caller while still
  holding the Store mutex. A throwing COMMIT never reaches caller mutation.
- Regression: `storage_commit_failure` asserts original history, pending count,
  unchanged caller ID, and successful retry on the same connection.
- Reproduction: configure as [verification](verification.md), then
  `ctest --test-dir build-native -R storage_commit_failure --output-on-failure`.
- Remaining risk: only the explicit success/failure contract is tested; a process
  crash after commit but before caller assignment still needs durable replay,
  already handled by the database/outbox. This is not exactly-once delivery.
- Design implication: database atomicity and in-memory publication are distinct
  boundaries. Keep caller mutation after the fallible durable operation.
- Fix commit: `1a53612`; [green log](evidence/commit-failure-green.txt):
  2 tests passed, 0 failed (0.11 s). No intentional implementation fault was
  introduced for this defect; the commit hook only triggers the failure path.

## MB-002 — Active transaction exposes a partial retention prune

**Controlled fault**, not an unexpected incident. Started from verified `1a53612`
in local worktree `build-investigation/worktree`, branch
`investigation/rollback-cleanup`. All data were disposable temporary databases.

1. Added `storage_partial_prune` in `55526eb`; all 3 tests passed
   ([baseline log](evidence/rollback-baseline.txt)). Fill capacity 3 with one
   acknowledged and two pending rows; reopen at capacity 2. The next append needs
   two evictions but can safely evict only one, so it must reject and restore all
   history. This covers a different boundary from a completely full outbox.
2. Deliberately removed catch-path ROLLBACK in `0abaaf8` (one fault).
   [Failing log](evidence/rollback-red.txt): original connection sees **2** rows,
   not 3. The test was then strengthened with an independent observer connection:
   it still sees **3** committed rows. This distinguishes incomplete transaction
   cleanup from durable deletion or wrong pending selection.
3. Root cause: DELETE succeeded inside BEGIN IMMEDIATE; capacity validation then
   threw; without ROLLBACK the writer kept its transaction-local deletion and
   lock. A retry would also encounter an already active transaction. Disk state
   is not yet lost, but same-process REST history would omit a row and polling
   could no longer append normally.
4. Restored ROLLBACK in `834c572`; the same connection now sees all 3 rows, then
   successfully acknowledges another row and appends at the lower capacity.
   [Green log](evidence/rollback-green.txt): all 3 tests pass. Broader checks here
   mean the portable storage suite, not unavailable Linux process checks.
5. Integrated baseline regression as `3062324`, plus observer assertions in the
   subsequent capacity-validation commit. Main's rollback implementation was
   already correct: **no implementation fix is claimed for MB-002 on main**.

Tradeoff: one explicit transaction keeps retention and insertion atomic; failure
cleanup is part of that contract. An RAII transaction helper is a possible future
simplification, not justified for this one transaction today. The fault branch
has not been pushed and ends in repaired code.

## MB-003 — Invalid Store capacity creates an unusable database

- Origin: genuine constructor validation gap in `a1cdda7`, unchanged in `1a53612`.
  Discovered while checking the new portable Store boundary. CLI already rejects
  invalid capacity, so impact is limited to direct library callers.
- Reproduce: construct `Store(temp_path, 0)` (also test -1). Expected:
  `invalid_argument` before any filesystem side effect. Actual: constructor
  succeeds and creates a schema, then samples cannot be accepted normally.
- Hypotheses: CLI validation might be sufficient, or Store might validate itself.
  Direct test bypasses CLI and confirms it does not. No cache or concurrency is
  involved. [Red log](evidence/capacity-red.txt) fails the constructor assertion.
- Cause: max_rows was assigned without validation; append assumes positive
  capacity in its eviction arithmetic.
- Fix: validate positive capacity before sqlite3_open. Test asserts both rejection
  and absence of a new database, for 0 and -1. A positive-capacity store is covered
  by every other scenario. No schema or valid CLI behavior changes.
- Fix commit `4691e65`; [green log](evidence/capacity-green.txt): 4 passed,
  0 failed/skipped. Final expanded seven-case run also passes; see the
  [verification ledger](verification.md).
  Prevention: validate invariants where state is owned, not only at an adapter.

## Host tooling observation

Adding filesystem tests initially produced Windows loader error `0xc0000139`
before a scenario could run (the first CTest attempt was stopped at 40 seconds).
`where.exe libstdc++-6.dll` found Git's DLL ahead of the GCC 16.1 runtime; PE import
inspection with `objdump -p` confirmed the executable's dynamic runtime dependency.
Prepending the selected compiler's bin directory to PATH made the same executable
pass without code changes. This is a host dependency-resolution issue, not a
storage defect. Reproduction instructions now call out matching compiler DLLs.
