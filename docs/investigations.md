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
- Fix commit and passing evidence: recorded below once verified.
