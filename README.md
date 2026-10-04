# Service Class Registry

Canonical registry of facility service classes.

A facility service class is what a facility promises about a workload: how
available it is, how much redundancy and cooling it has, how it is maintained
and recovered, how it is powered, where it may be placed, and what operational
obligations come with it. This repository turns that promise into typed,
digestible, immutable data that other components can bind decisions to, and it
refuses to accept a promise that contradicts itself.

The runtime is a C++20 library plus a `scr` command line tool. It has no
third-party runtime dependency, transmits no telemetry, and never dereferences
or simulates an authority it does not own.

## The core question

Given a set of facilities and the classes they claim to offer, what exactly does
class X generation G revision R *mean*, is that meaning self-consistent, and is
the decision that was made against it still bound to the exact definition it was
made against?

The registry answers that question with three things and nothing more:

1. a canonical, digest-addressed definition of each class revision;
2. a deterministic verdict on whether that definition is internally consistent,
   including what it inherits through composition;
3. an exact binding record plus a resolver that says whether a recorded binding
   is still the authoritative one, has been superseded, has been retired, or is
   no longer valid at all.

## What this repository owns

- Canonical service-class identity, lifecycle, version, generation, revision,
  and metadata.
- Structured service-class obligations and their deterministic canonical form.
- Compatibility and evolution rules between service-class revisions, expressed
  as explicit lineage and generation transitions.
- Composition of classes from exact parent revisions, including a proved-acyclic
  resolution and canonical flattened semantics.
- Refusal of contradictory obligations, with a closed rule set and stable rule
  identifiers.
- Deterministic comparison and explanation of two revisions.
- A versioned, integrity-checked durable store for the authoritative index,
  with real single-writer exclusion, atomic publication, fail-closed recovery,
  and rollback detection.
- Explicitly preserved unknown and unspecified state: an obligation that is
  declared as unknown stays unknown and is never collapsed into a default.

## What this repository explicitly does not own

- **Tenant identity.** Tenants are referenced, never defined, here.
- **Actual resource entitlement.** The registry states what a class requires; it
  does not grant, reserve, or account for resources.
- **Capacity measurements.** No measured capacity, utilisation, or headroom is
  stored or inferred.
- **Admission or placement decisions.** A class constrains placement; it does
  not choose a placement or admit a workload.
- **Maintenance execution.** Maintenance obligations describe windows, notice,
  and concurrency. No maintenance is scheduled or performed.
- **Incident response and recovery execution.** Recovery objectives are
  declared, not executed.
- **Power and cooling control.** Power and cooling obligations are declared
  properties, not control loops.
- **ASI/DFI runtime policy.** Adjacent authorities own those; the registry may
  reference them by name and digest, and validates only the reference form and
  the recorded digest.

References to external requirement sets, policy predicates, entitlement
profiles, and evidence sources are validated as *references*: identifier form,
optional authority identity, and optional exact content digest. The registry
never fetches, evaluates, or simulates the target. A reference recorded without
a digest is explicitly unbound, and the explanation says so; staleness cannot be
detected for an unbound reference, and it is never reported as fresh.

## Principal invariants

These hold for every accepted state, and each is exercised by the test suite.

1. **No opaque classes.** A revision must declare at least one typed obligation,
   and at least one of them must be `required`. A class with no obligations is
   refused.
2. **Immutability.** Once a revision is published, its content never changes.
   The digest of a revision is a function of its declared semantics only: the
   canonical content contains no wall-clock value, no lifecycle state, and no
   store identity.
3. **Explicit lineage.** A revision either starts a generation (genesis) or
   names its exact predecessor: generation, revision, and digest. A revision
   whose predecessor is not the current authoritative tip is refused as stale.
4. **Contiguous history.** Revisions within a generation are numbered from 1
   with no gaps and no reuse; generations are consecutive from 1.
5. **Strong binding.** A consumer binding carries class identity, generation,
   revision, and content digest. A binding without a digest cannot be verified
   and is refused at the API boundary.
6. **Deterministic canonical form.** Two revisions with the same semantics
   digest identically regardless of the order in which their obligations,
   references, or parents were declared.
7. **Acyclic composition.** Composition resolution walks exact parent revisions
   with an explicit in-progress set, is bounded in depth and in the number of
   resolved revisions, and refuses a cycle with the cycle path rather than
   recursing.
8. **No silent contradiction.** A contradictory obligation set is refused, not
   resolved by precedence. Where a lattice join exists (one requirement is
   strictly stricter, or one modality dominates), the join is taken and the
   dominated declaration is recorded as suppressed evidence and surfaced as an
   advisory.
9. **Explicit unknowns.** `unspecified` is a value-carrying statement that a
   property is unknown. It is preserved, never treated as absent, never treated
   as zero, and never satisfies a rule.
10. **Fail closed on durable state.** If durable state exists but cannot be
    verified, the store refuses to open. It never starts empty, merges, or
    guesses.
11. **One authoritative generation.** Recovery selects exactly one manifest
    generation (the highest sequence that the guard fence allows) or fails.
12. **Fenced authority.** A new control epoch is published atomically with the
    state that makes it authoritative, and the guard fence records it.

## Service class model

### Obligation keys

An obligation is a triple: a key from the closed registry below, a modality, and
a value whose kind, range, and enumerators are fixed by the key. Adding a key is
a wire-format change; existing numeric key values are never reused or
renumbered.

| Key | Id | Kind | Strictness |
| --- | --- | --- | --- |
| availability.target_ppm | 1 | integer [0, 1000000] | higher is stricter |
| availability.max_annual_downtime_seconds | 2 | integer [0, 31536000] | lower is stricter |
| redundancy.topology | 3 | enum(unspecified, none, n, n+1, n+2, 2n, 2n+1, distributed) | incomparable |
| redundancy.minimum_independent_fault_domains | 4 | integer [0, 64] | higher is stricter |
| redundancy.concurrent_fault_tolerance | 5 | integer [0, 63] | higher is stricter |
| redundancy.failover_mode | 6 | enum(unspecified, none, manual, automatic) | incomparable |
| redundancy.maximum_failover_seconds | 7 | integer [0, 86400] | lower is stricter |
| maintenance.mode | 8 | enum(unspecified, none, offline, rolling, online, concurrent-maintainable) | incomparable |
| maintenance.maximum_window_seconds | 9 | integer [0, 2592000] | lower is stricter |
| maintenance.minimum_notice_seconds | 10 | integer [0, 31536000] | higher is stricter |
| maintenance.concurrency_limit | 11 | integer [0, 1024] | higher is stricter |
| recovery.time_objective_seconds | 12 | integer [0, 31536000] | lower is stricter |
| recovery.point_objective_seconds | 13 | integer [0, 31536000] | lower is stricter |
| recovery.restore_mode | 14 | enum(unspecified, none, cold, warm, hot, continuous) | incomparable |
| power.feed_count | 15 | integer [0, 64] | higher is stricter |
| power.path_independence | 16 | enum(unspecified, single, dual-independent, multiple-independent) | incomparable |
| power.autonomy_seconds | 17 | integer [0, 86400] | higher is stricter |
| power.transfer_mode | 18 | enum(unspecified, none, manual, automatic) | incomparable |
| power.maximum_transfer_seconds | 19 | integer [0, 3600] | lower is stricter |
| cooling.mode | 20 | enum(unspecified, none, passive, mechanical, redundant-mechanical) | incomparable |
| cooling.independent_paths | 21 | integer [0, 64] | higher is stricter |
| cooling.maximum_ambient_millicelsius | 22 | integer [0, 60000] | lower is stricter |
| cooling.autonomy_seconds | 23 | integer [0, 86400] | higher is stricter |
| placement.minimum_isolation_domains | 24 | integer [0, 1024] | higher is stricter |
| placement.tenant_separation | 25 | enum(unspecified, none, logical, physical) | incomparable |
| placement.geographic_diversity | 26 | enum(unspecified, none, same-campus, same-region, multi-region) | incomparable |
| operations.monitoring_interval_seconds | 27 | integer [1, 86400] | lower is stricter |
| operations.change_control | 28 | enum(unspecified, none, standard, strict) | incomparable |
| operations.on_call_response_seconds | 29 | integer [0, 86400] | lower is stricter |
| operations.incident_notification_seconds | 30 | integer [0, 86400] | lower is stricter |

`scr keys` prints this table from the same descriptor table the runtime uses.

### Modalities

| Modality | Meaning |
| --- | --- |
| `required` | A hard requirement. Participates in every rule, and a contradiction among required obligations is a refusal. |
| `preferred` | A declared preference. Participates in rules, but a contradiction involving a preferred obligation is an advisory, not a refusal. |
| `permitted-degraded` | Degraded operation to this level is permitted. Advisory severity in rules. |
| `unspecified` | Explicitly unknown or undetermined. Must carry the value 0, never satisfies a rule, and is preserved in canonical form and explanation. |

### Composition and the merge algebra

A revision may compose exact parent revisions. Composition is a conjunction: the
composite must satisfy every parent as well as its own obligations.

- Parents merge by lattice join. When one value is strictly stricter under the
  key's strictness relation, the stricter value wins and the other is recorded
  as suppressed evidence with the reason `dominated-by-strictness`.
- When the modalities differ, the stronger modality wins and the weaker is
  recorded as `dominated-by-modality`.
- When two same-modality values are incomparable (different enumerators of an
  incomparable key, or different values of an equal-only key), the merge is
  refused as contradictory: nothing is chosen silently.
- A revision's own declaration may strengthen an inherited required obligation;
  it may not weaken it (`WeakenedRequirement`) and it may not replace it with
  `unspecified`.
- Every effective obligation carries provenance: the class, generation,
  revision, and digest of each declaration that contributed, marked as direct or
  inherited, plus the suppressed declarations and the reason each was dominated.

### Cross-key consistency rules

Rules are evaluated in a fixed order over the effective (flattened) obligations.
Every violation that applies is reported; the first refusal in rule order is the
primary failure and the rest are retained as secondary diagnostics.

| Rule | Refused when | Rationale |
| --- | --- | --- |
| R-01 | rolling, online, or concurrent-maintainable maintenance with a zero concurrent fault tolerance | maintenance without tolerance takes the service down |
| R-02 | concurrent-maintainable maintenance without a declared topology other than none | concurrent maintenance needs redundant paths |
| R-03 | tolerated concurrent faults greater than or equal to the independent fault domain count | more tolerated faults than domains is unsatisfiable |
| R-04 | topology none with more than one independent fault domain | none means a single path |
| R-05 | manual or automatic failover without a maximum failover time | failover without a bound is unbounded downtime |
| R-06 | failover mode none with a non-zero maximum failover time | nothing fails over |
| R-07 | recovery time objective greater than the annual downtime allowance | one recovery would exceed the yearly budget |
| R-08 | annual downtime allowance greater than the availability target permits | two statements of the same budget disagree |
| R-09 | power transfer with fewer than two feeds | there is nothing to transfer to |
| R-10 | independent power paths with fewer than two feeds | independent paths need two sources |
| R-11 | manual or automatic transfer without a maximum transfer time | transfer without a bound is unbounded |
| R-12 | transfer mode none with a non-zero maximum transfer time | nothing transfers |
| R-13 | a non-zero independent cooling path count with cooling mode none | no cooling path exists |
| R-14 | more than one independent cooling path without redundant mechanical cooling | independent paths imply redundant machinery |
| R-15 | physical tenant separation without at least two isolation domains | physical separation needs two domains |
| R-16 | geographic diversity without at least two isolation domains | diversity needs two domains |
| R-17 | hot or continuous restore without a recovery point objective | continuous restore without an RPO is unspecified data loss |

The rule engine reports the rule identifier, the participating obligations, the
severity, and a deterministic explanation string.

### Determinism

- Obligations iterate in ascending key order; references by kind, target,
  digest, and authority; composition edges by parent class and revision.
- Insertion order never changes the canonical bytes or the digest, which is
  tested with randomised shuffles.
- Given the same authoritative inputs, every API result is identical. There is
  no randomness, no clock input, and no locale-sensitive formatting in the
  canonical form.

## Authority: generations, revisions, epochs, and fencing

- **Revision** (`u32`): a revision number within a generation. Immutable once
  published, numbered contiguously from 1.
- **Generation** (`u64`): an incarnation of a class identity. A new generation
  starts at revision 1 with genesis lineage and must be requested explicitly.
  Publishing a new generation fences the previous generation: an old binding
  resolves as `generation-superseded` and must be re-established rather than
  inherited.
- **Epoch** (`u64`): the control epoch of the store. Adopting a new epoch is
  published atomically with the state that makes it authoritative, is recorded
  in the guard fence, and fences a writer that still holds the old epoch
  (`StaleAuthority`).
- **Sequence** (`u64`): the manifest commit counter. Strictly increasing; the
  guard fence records the highest sequence ever published.
- **Digest**: SHA-256 over the complete canonical frame, which binds class
  identity, generation, revision, lineage, obligations, references, composition
  edges, and metadata.

What invalidates authority:

| Change | Effect on existing bindings |
| --- | --- |
| New revision of the same generation | Previous binding becomes `superseded`; the new tip is reported |
| New generation (re-founding) | Previous generation becomes `generation-superseded` |
| Retirement of the class | Binding becomes `retired`; the revision stays verifiable and the digest is unchanged |
| Restored older manifest inside the store | Refused at open as `RollbackDetected` |
| New control epoch | Writers holding the old epoch are refused as `StaleAuthority` |

Binding resolution is a total function with these states: `fresh`,
`superseded`, `retired`, `generation-superseded`, `unknown-class`,
`unknown-revision`, `digest-mismatch`. Verification order is class,
generation, revision, digest, lifecycle, so the diagnostic names the first thing
that does not match, and every resolution carries the authoritative tip when one
exists.

## Lifecycle model

| State | Meaning |
| --- | --- |
| active | The class accepts new revisions of its current generation. |
| retired | The class accepts no new revision of its generation. History, digests, and bindings remain verifiable. |

Retirement requires a non-empty reason and a recorded time. It is idempotent for
lost-response safety: retiring an already retired class succeeds without
changing the recorded retirement. A retired class may only be re-founded as an
explicitly requested new generation, which starts at revision 1 with genesis
lineage and reactivates the class while keeping the previous generation in
history.

**Idempotent replay precedes staleness.** Re-publishing content that is already
published returns success with the exact existing reference and an
`IdempotentReplay` advisory, before any lineage or tip check runs. This is what
makes a lost response safe to retry, and it is tested at the library, CLI, and
crash-recovery level.

## Canonical representations

### Canonical binary frame (SCRF, version 1)

Every durable object is a frame:

    [ 0.. 3] magic            S C R F
    [ 4.. 5] format_version   u16, little endian
    [ 6]     kind             u8: 1 revision content, 2 store manifest, 3 store guard
    [ 7]     flags            u8, reserved, must be zero
    [ 8..11] payload_length   u32, little endian
    [12..  ] payload
    [16+len] crc32            u32 over bytes [0, 12 + payload_length)

The total length must be exactly 16 + payload_length. Shorter or longer input is
refused: declared lengths are never trusted, nothing is allocated from an
unvalidated length, and trailing garbage is never ignored. The digest of a frame
is the SHA-256 of the complete frame, which is why the frame rather than the
payload is the unit that gets bound.

Revision payloads are length-prefixed and versioned, with every field bounded:
obligation count, reference count, composition count, identifier lengths, text
lengths, and enumerator domains are all validated on decode. Decoding rejects
undefined keys, undefined enumerators, undefined modalities, undefined reference
kinds, non-zero reserved bytes, and trailing bytes.

### Canonical text form (SCX, version 1)

    scr-class 1
    class facility.core/dc-hall-a
    generation 1
    revision 3
    lineage 1.2@<64 hex characters>
    obligation redundancy.topology required n+1
    obligation availability.target_ppm required 999000
    parent facility.core/power-train @ 1.2@<64 hex characters>
    reference policy-predicate facility.policy/tenant-floor @<64 hex|unbound> authority=scr-primary
    text title "Hall A"

The header is mandatory and first. Directives after it may appear in any order;
obligations, references, and parents are canonicalized deterministically. The
parser is strict: unknown directives, unknown keys, unknown enumerators,
duplicate directives, duplicate keys, malformed references, out-of-range
integers, integer overflow, over-long lines, invalid UTF-8, control characters,
and unsupported escapes are refused with the line number and the reason. The
emitter produces one canonical byte sequence per revision, and the digest of a
re-emitted revision is identical to the original, which the tests assert.

`scr class export --format hex` prints the canonical frame as lowercase hex for
consumers that want the exact bytes rather than the text form.

## Durable store

### Layout

    <root>/LOCK              real OS single-writer exclusion
    <root>/manifest          canonical frame, kind 2: the commit point
    <root>/guard             canonical frame, kind 3: sequence and epoch fence
    <root>/records/<hex>.scr immutable revision frames, named by their own SHA-256
    <root>/staging/          staging area for atomic publication

The manifest holds the authority identity, control epoch, commit sequence, write
time, and one record per class: identity, lifecycle state, generation, tip,
tip digest, publication and retirement times, retirement reason, and the full
history of exact (generation, revision, digest) bindings with publication times
and generation markers. The manifest is the single reachability root for the
record files.

### Commit protocol and the commit point

1. Every published revision that has no record file yet is staged into
   `staging/`, flushed to the device, read back, and verified against its
   digest.
2. All staged records are published by atomic rename into `records/`. A record
   file is content addressed, so an existing file is never replaced.
3. The new manifest is encoded, staged, flushed, read back, and verified.
4. The staged manifest is renamed onto `manifest`. **This rename is the commit
   point.** Before it, the previous manifest is authoritative; after it, the new
   one is.
5. The guard fence is written with the new sequence, manifest digest, and epoch,
   after the publication it protects.

The guard is deliberately advanced *after* the manifest, so a manifest ahead of
the guard is the expected crash window, while a guard ahead of the manifest is a
rollback and is refused. A commit whose index is unchanged is detected by an
index digest that excludes the sequence and write time, and becomes an
idempotent no-op.

Because every commit rewrites the manifest, commit cost grows linearly with the
number of published revisions. The manifest bound is 64 MiB, which is on the
order of 300,000 revisions for this record shape. The registry is designed for
facility service classes, not for per-tenant per-object data.

### Recovery and fail-closed rules

On open, in this order:

1. The store root must exist, be a directory, and not be a reparse point
   (symlink, junction, or mount point).
2. The exclusive (read-write) or shared (read-only) OS lock is taken on
   `LOCK` before anything is read.
3. The guard fence and the manifest are read, their frame structure, CRC,
   declared lengths, and exact sizes are validated, and the manifest is rehashed.
4. The fence is checked: a manifest older than the guard (by sequence or epoch)
   is `RollbackDetected`; a guard with the same sequence but a different
   manifest digest is `CorruptStore`. A guard behind the manifest is repaired in
   read-write mode and accepted in read-only mode.
5. Every referenced record file is read, rehashed, decoded, and checked against
   the identity recorded in the manifest; the index is rebuilt and revalidated
   (identity, digests, contiguity, lineage links, tip, retirement shape).
6. Only then is the registry exposed. Read-only opens perform no mutation at
   all, not even staging cleanup, and obey exactly the same integrity and
   rollback rules.

If any step fails, open fails closed with a specific code. Durable state that
exists but cannot be verified is never replaced by an empty state.

### Rollback protection and its threat model

The guard fence detects a stale copy of the manifest inside the store directory:
a restored older manifest, an interrupted commit that published a manifest and
died before the guard, or a partial copy-back. It does not, and cannot, detect a
wholesale restore of the entire store directory from a backup, because the fence
travels with the state. Protecting against that requires an external monotonic
witness; this repository documents the limit rather than claiming the property.

### Compaction

`scr store compact` removes staging residue and record files that the manifest
does not reference. It verifies the store before and after, refuses to compact a
store that does not verify, and never deletes a file whose name it does not
recognise as a record (a foreign file is reported by `verify` instead). The
result is always a state a normal reader accepts.

### Concurrency model and lock audit

The library has no in-process locks, no threads, and no atomics: there is
exactly one mutex-free object model.

- **Registry and Store are single threaded per instance.** One instance is owned
  by one thread. Sharing an instance across threads requires external
  synchronisation; nothing in the library does it implicitly, and no callbacks
  are invoked under any internal lock because there are none.
- **Cross-process exclusion is real and is enforced by the operating system.**
  Read-write opens take an exclusive handle on `LOCK`; read-only opens take a
  shared handle that allows other readers and denies writers. The kernel
  releases the lock when the holding process dies, which the process test proves
  by killing the holder abruptly and immediately taking the lock again.
- **One lock, so no lock ordering exists.** The file lock is taken before any
  file operation and released only when the `Store` is destroyed, so lock
  inversion, lock upgrade, and nested acquisition are impossible by
  construction.
- **No blocking I/O happens under any other lock**, and there is no other lock.
- **No asynchronous completion exists**: every operation completes before it
  returns, so there is no stale callback to fire after authority changes.
- The audit is performed by inspection and by the real multi-process tests
  rather than by reasoning about a lock hierarchy that does not exist.

## Errors, refusals, and precedence

### Error taxonomy

Every failure is a `Status` with a stable `ErrorCode`, a subject, a
deterministic detail string, and any secondary diagnostics that were observed
before the decision was made. The codes are never renumbered.

`Ok`, `InvalidArgument`, `InvalidIdentifier`, `InvalidEnum`,
`InvalidTextEncoding`, `OutOfRange`, `BoundExceeded`, `DuplicateIdentity`,
`DuplicateObligation`, `ContradictoryObligations`, `MissingRequirement`,
`WeakenedRequirement`, `CycleDetected`, `ImmutableRevision`,
`LineageMismatch`, `StaleTip`, `UnknownClass`, `UnknownRevision`,
`RetiredClass`, `DigestMismatch`, `UnsupportedVersion`, `TruncatedInput`,
`CorruptStore`, `RollbackDetected`, `StoreBusy`, `StoreNotInitialized`,
`AlreadyInitialized`, `Overflow`, `UnauthorizedTransition`, `IoFailure`,
`UnknownField`, `ParseError`, `Internal`, and the advisory-only codes
`MergedStricter`, `DominatedByModality`, `ExplicitUnspecifiedOverridden`,
`StrengthenedInheritance`, `IdempotentReplay`, `HistoricalRetiredParent`,
`GuardRepaired`, `OrphanRecordsPresent`, `StaleAuthority`.

Advisory codes never appear as the code of a failed `Status`; they describe a
deterministic merge or lifecycle decision that is surfaced for explainability.

### Publication precedence

When more than one thing is wrong, the first applicable stage wins and the rest
are retained as secondary evidence:

1. structural validation of the canonicalized content (identity, counters,
   lineage shape, obligation shape, entry bounds, duplicate identities,
   canonical ordering);
2. content digest, then idempotent replay before any staleness check;
3. composition resolution and merge (identity, retirement of a parent, cycles,
   depth and revision bounds, dominance, weakening);
4. cross-key rules in fixed rule order, first refusal as the primary;
5. class state, then lineage, then tip continuity;
6. commit into the index and, for the store, durable publication.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 2 | usage error: unknown command or option, missing or malformed option value |
| 3 | refusal: validation, contradiction, lineage, cycle, or weakened requirement |
| 4 | not found: unknown class or unknown revision |
| 5 | stale binding: superseded, retired, generation-superseded, or digest mismatch |
| 6 | store integrity or exclusion: busy lock, corrupt or uninitialised store, rollback, stale authority |
| 7 | I/O failure |

## Command line interface

    scr version
    scr rules
    scr keys
    scr validate      --file <scx> [--json]
    scr store init    --store <dir> --authority <id> [--epoch <n>] [--at <unix-seconds>]
    scr store info    --store <dir> [--json]
    scr store verify  --store <dir> [--json]
    scr store compact --store <dir> [--json]
    scr store adopt-epoch --store <dir> --epoch <n> [--at <unix-seconds>] [--json]
    scr store lock-probe --store <dir> --hold-ms <n>
    scr class publish --store <dir> --file <scx> [--allow-new-generation] [--allow-retired-parents]
                      [--at <unix-seconds>] [--json]
    scr class show    --store <dir> --class <id> [--json]
    scr class explain --store <dir> --class <id> [--json]
    scr class history --store <dir> --class <id> [--json]
    scr class compare --store <dir> --class <id> --from <generation.revision[@digest]>
                      --to <generation.revision[@digest]> [--json]
    scr class retire  --store <dir> --class <id> --reason <text> [--at <unix-seconds>] [--json]
    scr class export  --store <dir> --class <id> [--generation <n>] [--revision <n>]
                      [--format scx|hex] [--out <file>]
    scr bind          --store <dir> --class <id> [--generation <n>] [--revision <n>] [--digest <hex>] [--json]
    scr check         --store <dir> --class <id> --generation <n> --revision <n> --digest <hex> [--json]
    scr bench         [--iterations <n>] [--json]

A complete session:

    scr store init --store C:\srv\scr --authority scr-primary --at 1700000000
    scr class publish --store C:\srv\scr --file hall-a.scx --at 1700000100
    scr class show --store C:\srv\scr --class facility.core/dc-hall-a --json
    scr bind --store C:\srv\scr --class facility.core/dc-hall-a
    # -> binding facility.core/dc-hall-a 1.1@<digest>
    scr check --store C:\srv\scr --class facility.core/dc-hall-a \
        --generation 1 --revision 1 --digest <digest>
    # exit 0 while it is the tip, exit 5 once superseded or retired
    scr class explain --store C:\srv\scr --class facility.core/dc-hall-a
    scr class compare --store C:\srv\scr --class facility.core/dc-hall-a --from 1.1 --to 1.2
    scr class retire --store C:\srv\scr --class facility.core/dc-hall-a --reason "hall decommissioned"
    scr store verify --store C:\srv\scr --json
    scr store compact --store C:\srv\scr

`scr store lock-probe` acquires the real write lock and holds it for the
requested time; it exists so that operators and tests can inspect exclusion
behaviour in a controlled way.

### Crash-consistency fault injection

Setting `SCR_FAULT_INJECT` to one of the commit stage names terminates the
process immediately (without unwinding, flushing user buffers, or running
destructors) at that stage:

| Value | Stage |
| --- | --- |
| `records-staged` | revision records written to staging and verified, not yet published |
| `records-published` | revision records renamed into `records/`, manifest not yet staged |
| `manifest-staged` | manifest staged, flushed, and verified, not yet renamed |
| `manifest-published` | manifest renamed (the commit point), guard not yet written |
| `guard-staged` | guard staged and verified, not yet renamed |
| `guard-published` | guard renamed |

The facility is off unless the variable is set, and it is the mechanism the
crash-recovery tests use to prove that every stage is either fully published or
completely invisible to the next open.

## Library integration

    #include "scr/registry.hpp"
    #include "scr/store.hpp"
    #include "scr/text.hpp"

    // Parse the canonical text form. Refusals carry a stable code and a reason.
    auto content = scr::parse_scx(source_text);
    if (!content.ok()) { report(content.status()); }

    auto store = scr::Store::open(root, scr::StoreMode::ReadWrite);
    if (!store.ok()) { report(store.status()); }

    scr::PublishOptions options;
    options.at = scr::Timestamp::from_unix_seconds(1700000100).value();
    auto published = store.value().publish(content.value(), options);
    if (!published.ok()) { report(published.status()); }

    // Bind a decision to the exact revision.
    scr::ClassBinding binding;
    binding.class_id = content.value().class_id;
    binding.revision = published.value().publish.reference;

    // Later, resolve it: fresh, superseded (with the new tip), retired,
    // generation-superseded, unknown-class, unknown-revision, or digest-mismatch.
    auto resolution = store.value().registry().resolve(binding);

    // Explain the authoritative definition, including provenance and every
    // merge decision that was taken.
    auto explanation = store.value().registry().explain(content.value().class_id);

```text
Public headers
  scr/status.hpp       error taxonomy, Status, diagnostics
  scr/result.hpp       Result<T> and Result<void>
  scr/digest.hpp       SHA-256, CRC-32, Digest
  scr/types.hpp        identifiers, generations, revisions, epochs, timestamps, bindings
  scr/obligation.hpp   obligation key registry, modalities, obligation sets
  scr/revision.hpp     revision content, lineage, references, composition edges
  scr/canonical.hpp    canonical frame and payload codecs
  scr/text.hpp         canonical text form (SCX)
  scr/contradiction.hpp rule engine
  scr/composition.hpp  acyclic composition, flattening, provenance
  scr/registry.hpp     in-memory registry, publish, resolve, compare, explain, retire
  scr/store.hpp        durable store, commit, recovery, verification, compaction
  scr/report.hpp       deterministic text and JSON renderers
  scr/platform.hpp     the OS primitives the store needs
```

## Building, installing, and consuming the package

    cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/rel
    ctest --test-dir build/rel --output-on-failure
    cmake --install build/rel --prefix /opt/scr

Installed layout: headers under `include/scr`, the static library, the `scr`
executable, and a CMake package under `lib/cmake/ServiceClassRegistry`.

An out-of-tree consumer uses only the installed package:

    find_package(ServiceClassRegistry CONFIG REQUIRED)
    target_link_libraries(my_component PRIVATE Summon::ServiceClassRegistry)

`examples/consumer` is such a consumer. It opens or creates a store, publishes a
revision, verifies the durable state, resolves the binding again after a close
and reopen, publishes a second revision bound to the authoritative record, and
compares the two revisions. Running it twice against the same directory also
exercises restart and idempotent replay.

CMake options: `SCR_BUILD_TESTS`, `SCR_BUILD_CLI`, `SCR_WARNINGS_AS_ERRORS`
(default ON), `SCR_ENABLE_ASAN` (default OFF). An unrecognised
`CMAKE_BUILD_TYPE` is a hard configuration error rather than a silently
unoptimised build.

## Validation performed

Everything in this section was run on the release host: Windows 11 x64, MSVC
19.44 (Visual Studio 2022 Build Tools), Ninja 1.13.2, CMake 4.3.2, 16 logical
processors. Warning policy `/W4 /WX /permissive-` on every first-party target.

### Builds and suites

| Configuration | Build | Unit suite | Process suite |
| --- | --- | --- | --- |
| Release (`/O2 /Ob2 /DNDEBUG`) | clean, zero warnings | 31 tests, 1696 checks, 0 failures | 4 tests, 218 checks, 0 failures |
| Debug (`/Zi /Ob0 /Od /RTC1`) | clean, zero warnings | 31 tests, 1696 checks, 0 failures | 4 tests, 218 checks, 0 failures |
| Debug + AddressSanitizer (`/fsanitize=address`) | clean, zero warnings | 31 tests, 1696 checks, 0 failures | 4 tests, 218 checks, 0 failures |

AddressSanitizer was confirmed to be genuinely active rather than silently
ignored: `dumpbin /dependents` on the instrumented `scr.exe` lists
`clang_rt.asan_dynamic-x86_64.dll`, and the instrumented binaries ran the full
suite with no report.

What the unit suite covers: SHA-256 known-answer vectors (including the one
million character vector) and CRC-32; strict digest and identifier parsing;
UTF-8 validation including overlong, surrogate, truncated, and out-of-range
sequences; counters that refuse to wrap; timestamps; obligation creation bounds
and enumerators; insertion-order independence of the canonical form; exact
round-trips of the canonical payload and frame; every single-bit mutation and
every truncation of a frame refused; all 17 rules in refusal and advisory form;
the merge lattice including incomparable refusal, weakening refusal, and
`unspecified` handling; cycle, self-reference, diamond, depth, and revision
budget behaviour; registry lifecycle, lineage, tip continuity, idempotent
replay, retirement, and re-founding; every binding state; comparison and
explanation determinism; canonical text round-trips and a hostile-input corpus;
store round-trip, read-only refusal, corruption and truncation sweeps, rollback
detection, guard repair, compaction, hostile manifest payloads, reserved device
names, junctions, and long paths; seeded property tests over a randomised
registry state machine, a store round-trip, and shuffled canonical text; and
adversarial payload bounds, duplicate identities, and revision shape limits.

What the process suite covers, with real independent OS processes:

- CLI end to end: init, validate, publish, idempotent replay, show, explain,
  bind, check (fresh and superseded), compare, export and re-validate the
  exported text, retire, post-retirement check, verify, compact, info.
- Usage and refusal exit codes across eleven command lines plus a contradictory
  publication.
- Writer exclusion: while a holder process has the lock, another process is
  refused with `StoreBusy` and exit 6; after the holder is killed abruptly, the
  lock is available again and a publication succeeds, proving kernel lock
  release on death.
- Crash consistency at all six fault-injection stages: the process dies with
  exit 137, the store still verifies, the class is either absent or exactly the
  crashed revision, and retrying the same publication completes it, either as a
  fresh publication or as an idempotent replay, after which the store still
  verifies.

### Adversarial hardening performed

- Single-bit mutation of every bit of a canonical frame (all refused).
- Truncation at every length (all refused), trailing bytes (refused), absurd
  declared lengths (refused before allocation).
- Corruption of the manifest, guard, and record files: bit flips, truncation,
  deletion, foreign files, wrong frame kind (each refused with a specific code).
- Rollback: an older manifest inside the store (refused as `RollbackDetected`),
  a forged guard with a newer epoch (refused).
- Path handling: empty paths, embedded NUL, invalid UTF-8, a store root that is
  a regular file, a missing store, a Windows reserved device name, a directory
  junction as the store root, and a store root deeper than `MAX_PATH`.
- Hostile canonical text: unknown directives, duplicate directives, duplicate
  keys, unknown enumerators, case tampering, malformed references, bad escapes,
  control characters, invalid UTF-8, over-long lines, integer overflow.
- Integer boundaries: exhausted generation, revision, and sequence counters.
- Duplicate identities in obligations, references, composition edges, and class
  restoration.
- Compaction never deletes a file it does not recognise, and refuses to run on a
  store that does not verify.

### Hardening defects found and fixed during this work

- **Win32 error codes were read lazily.** `GetLastError()` was called inside the
  error-formatting helper, after other Win32 and CRT calls had already run, which
  reported a misleading code and hid the real cause. Error codes are now captured
  at the failure site and passed explicitly; this is what exposed the directory
  creation defect below.
- **Directory existence was checked with `std::filesystem`.** For paths beyond
  `MAX_PATH` that check silently reported "does not exist", so a long-path store
  failed with "already exists" from `CreateDirectoryW`. Existence and
  enumeration now use the extended-length-prefix aware Win32 helpers, with
  component-by-component creation.
- **Retirement could store an unverifiable record.** Retiring with an unset
  timestamp produced an index that its own verifier rejected. Retirement now
  requires a recorded time at the API boundary.
- **Metadata text could carry control characters into the canonical form.** The
  parser accepted `\n`, `\r`, and `\t` escapes that the value model then
  refused, so an accepted escape produced a value that could not be stored. The
  parser now refuses those escapes explicitly.
- **Quoted text was split on whitespace.** A quoted metadata value containing
  spaces was tokenised into several tokens and rejected. The tokeniser now keeps
  quoted regions (including `note="..."` options) together.
- **Provenance carried a null digest.** Explanations told a consumer which
  revision contributed an obligation but not which content digest to bind to.
  Provenance now carries the exact declaring digest.
- **An invalid `CMAKE_BUILD_TYPE` produced an unoptimised build.** A typo or a
  bad scripted value silently produced a build with no optimisation and no
  configuration flags at all. Configuration now fails loudly.
- **Commit cost was quadratic in the history size.** Every commit re-read and
  rehashed every existing record file. Existing records are now trusted after the
  open-time verification (and are re-verified by every open and by
  `scr store verify`), which keeps a commit proportional to the new revisions.

## Benchmarks

Measured with `scr bench --iterations 200` on the release host described above,
Release build, single host, no other load. **These are single-host measurements
of this build on this machine and are not a comparison with any other system or
build.** Provenance labels: REAL means the measurement includes the real durable
or operating-system path; SYNTHETIC means in-memory or pure computation.

| Operation | Provenance | Iterations | Per completed operation | Rate |
| --- | --- | --- | --- | --- |
| canonical_digest (encode + SHA-256) | SYNTHETIC | 200 | 4.68 microseconds | 213,858 ops/s |
| resolve_fresh_binding | SYNTHETIC | 200 | 137 nanoseconds | 7,299,270 ops/s |
| explain_authoritative (flatten + rule evaluation) | SYNTHETIC | 200 | 24.7 microseconds | 40,494 ops/s |
| publish_durable (stage, flush, read back, verify, publish records, manifest, guard) | REAL | 200 | 21.1 milliseconds, 386 bytes written | 47.3 ops/s |
| open_verified_cold (manifest, guard, and every record verified) | REAL | 20 | 12.6 milliseconds | 79.1 ops/s |

Methodology: the benchmark creates its own store in the temporary directory,
publishes a base class and a composed class, measures the in-memory operations
against those, then publishes 200 successive revisions through the full durable
path, and finally reopens the resulting store (3 classes, 202 revisions) with
full verification 20 times. Each measurement times completed operations only: a
durable publication is complete when the manifest rename and the guard write have
finished, and a store open is complete when every referenced record has been
rehashed and the index has been revalidated. The store directory is removed
afterwards. The numbers include the growth of the index across the run, which is
the honest cost of publishing into this store design.

## Platform support and unvalidated behaviour

- **Windows x64 is the validated platform.** Everything in "Validation performed"
  above was executed there.
- **The POSIX backend exists but is not validated in this release.** The library
  contains a POSIX implementation of the platform layer (bounded reads, staged
  writes with `fsync`, `rename` and `link` publication, `flock` exclusion,
  `SIGKILL` fault injection), and CMake selects it automatically on non-Windows
  hosts. The release host has no POSIX toolchain, so it was neither compiled nor
  exercised here. It must be treated as unvalidated until it is built and tested
  on such a host.
- **The process-level tests are implemented for Windows.** They report that
  clearly on other platforms rather than pretending to pass.
- **No physical hardware behaviour is measured or claimed.** Availability,
  redundancy, power, and cooling are declared semantics; nothing here measures a
  real facility, and no benchmark result implies hardware performance.
- **Rollback protection is store-local.** See "Rollback protection and its
  threat model": restoring an entire store directory from a backup is not
  detectable by the in-store guard fence.
- **Commit cost is linear in the number of published revisions**, because the
  index is rewritten atomically on every commit. This is a deliberate trade of
  scalability for a single, verifiable commit point.

## Repository layout

    include/scr/       public headers
    src/               library, CLI, benchmark, platform backends
    tests/             unit, property, adversarial, and process suites
    examples/consumer/ out-of-tree consumer used to validate the installed package
    cmake/             CMake package configuration template
    CMakeLists.txt     single build description for library, CLI, and tests

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.