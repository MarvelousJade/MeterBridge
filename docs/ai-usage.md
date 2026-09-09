# AI development log

Tool: Codex. Date: September 9, 2026.

| Work | Validation and correction |
|---|---|
| Generated C++ transport, persistence, REST and MQTT code | Compiled with warnings treated as errors; tested real processes and protocols. |
| Generated fault-injection scenarios | Offline-broker test exposed an immediate `connect_async` failure; moved retries out of startup. |
| Reviewed retention and shutdown paths | Fixed lowering the row limit after restart; made unexpected subprocess exits fail tests. |
| Checked resource ownership | Ran AddressSanitizer, UndefinedBehaviorSanitizer and leak detection. |

No time-saving baseline was recorded, so no productivity percentage is claimed.
Human review and physical-board validation are not recorded as completed.
