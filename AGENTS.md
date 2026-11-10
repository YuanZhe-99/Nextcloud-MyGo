# AGENTS.md

Operating guide for agents working on **Nextcloud-MyGo**. This file holds **only** rules about how
to work here. Everything describing what the code *is* or *does* lives in `doc/en-us/` — see
[Where to read what](#where-to-read-what).

Nextcloud-MyGo is a Windows desktop client for Nextcloud file sync that reuses the Nextcloud
Desktop sync engine and adds network awareness: one account, several trusted addresses of the same
server (WAN, LAN, VPN), and safe switching between them. It is two processes shipped as one
verified combination: a Go + MyGo UI, and **SyncD**, a C++/Qt background process of the current
user that links the upstream `libsync` and owns sync, credentials, endpoint choice and recovery.
They talk over a Named Pipe with JSON-RPC 2.0. Windows 11 x64 and native ARM64 for the first
release. Treat the user's message as the change request: plan, implement, verify, report.

## Reading order

When you need to understand code, read in this order and stop as soon as you have what you need:

1. **`doc/en-us/`** — start here, always. `functions/INDEX.md` to find the right package or
   module; the concept docs for behavior.
2. **`PLAN.md`** — while it is active, for anything not yet implemented: architecture, algorithms,
   the IPC contract, milestones and acceptance.
3. **Comments in the source** — the Function Explanation Layer on each declaration.
4. **The implementation** — only when the docs and comments are insufficient, or when you must
   confirm actual behavior before changing it.

Do not jump straight to reading source bodies. Where docs and code disagree on something you are
about to change, verify against the code, then fix the docs in the same commit.

## Where to read what

| Question | Read |
|---|---|
| Design, algorithms, IPC contract, milestones, acceptance (while the plan is active) | `PLAN.md` |
| Interface screens, wording and design tokens (reference for the MyGo UI) | `design/` |
| The interface: pages, setup, Settings, tray, closing, updates, languages, the Mock | `doc/en-us/gui.md` |
| Verified upstream contracts (Nextcloud Desktop, Nextcloud server APIs, Qt, MyGo) and quirks | `doc/en-us/upstream.md` |
| Reused third-party code, licenses and obligations | `doc/en-us/third-party.md` |
| What is implemented and verified per platform and architecture | `doc/en-us/platform-notes.md` |
| Backend work-package progress, dependency audit and test isolation | `doc/en-us/backend-progress.md` |
| URL request surfaces and endpoint-recovery audit | `doc/en-us/url-usage-audit.md` |
| Implemented IPC contract and connected daemon scope | `doc/en-us/ipc.md` |
| Account creation, login sessions and native credential ownership | `doc/en-us/accounts.md` |
| Engine adapter ownership and serial scheduling | `doc/en-us/engine.md` |
| Environment, commands, dev loop | `doc/en-us/development.md` |
| English→Chinese terminology | `doc/en-us/translation-guide.md` |
| Past releases and the upstream versions they used | `doc/en-us/version-history.md` |
| Per-package and per-module roles | `doc/en-us/functions/INDEX.md` |

New concept docs (architecture, the IPC protocol, endpoints and switching, scheduling, file
safety, security, packaging, CI/CD, the audits M0 produces) are added with the code or results they
describe; add their rows here in the same commit.

## Division of work

Two kinds of agents work here, following `PLAN.md` §17–§19:

- The **backend agent** owns SyncD (`backend/`), the upstream snapshot and its patches
  (`third_party/nextcloud/`, `patches/nextcloud/`), `build-lock.json`, the protocol (`protocol/`),
  C++ tests and Windows packaging. It starts at M0 / B01. SyncD must support the platforms'
  modern cloud-file frameworks — the Windows Cloud Files API and macOS File Provider (`PLAN.md`
  §2.3): not enabled in the first release, but the data model and the `platform/<os>/` layer
  leave room for them from M1, and M6 delivers them.
- The **frontend agent** owns the MyGo UI: the Go module at the repository root (`cmd/`,
  `internal/`, `locales/`, `resources/`), its layout and its choice of Native or Web UI.
- The interface exists and runs against `syncd.Mock` (`doc/en-us/gui.md`). What it expects of
  SyncD is `internal/syncd`: the `Client` methods and the snapshot types, a draft of `PLAN.md`
  §13 written from the interface's side. B02 turns it into `protocol/` and replaces the Mock with
  the Named Pipe client; where the protocol has to differ, change `internal/syncd` and the
  views in the same change set. The Mock's tests encode invariants SyncD must keep too (a
  folder's own pause survives Resume all, a decision is answered once, an update waits for a
  safe point).
- `protocol/` is the only interface between them. A change to a method, event, error or field
  meaning updates the schemas, examples and fixtures, both sides' contract tests and the protocol
  version in one change set; neither side reaches past it.

`PLAN.md` §19.1 suggests a layout; in this repository it maps as follows:

| `PLAN.md` path | Repository path | Why |
|---|---|---|
| `vendor/nextcloud/` | `third_party/nextcloud/` | a root `vendor/` would switch the Go module into vendor mode |
| `frontend/` | the Go module at the root (`cmd/`, `internal/`, `locales/`) | `mygo build` and the shared tools expect the module at the root |
| `docs/*.md` (audits, feasibility) | `doc/en-us/*.md`, mirrored in `doc/zh-cn/` | one documentation tree, mirrored |
| `docs/decisions/`, `docs/validation/` | `doc/en-us/decisions.md`, `doc/en-us/platform-notes.md` | same |

Everything else (`backend/`, `patches/`, `protocol/`, `tests/`, `packaging/`, `build-lock.json`)
keeps the plan's path.

## Required workflow

1. Treat the user's message as the modification request.
2. Before editing, fetch the remote(s) and check whether the local branch is behind. Resolve any
   divergence before starting.
3. Check the upstreams per [Upstream versions](#upstream-versions).
4. Read per [Reading order](#reading-order). Find the milestone and work package (`PLAN.md` §18,
   §19.2) the change belongs to; do not start work whose dependencies are not done.
5. Keep changes scoped. Do not revert unrelated work in the tree.
6. Update documentation in the same change set — see
   [Documentation maintenance](#documentation-maintenance).
7. Verify — see [Verification](#verification).
8. Report briefly, in English and Chinese, with what `PLAN.md` §19.3 asks: what changed, the
   invariants (`INV-xx`) and tests it covers, upstream patches added and why, what was verified
   where (with evidence), what is still unverified, any deviation from the plan, the current
   version and the configured remotes.
9. Ask before pushing. The user decides when to release.

## Documentation maintenance

**Docs are the primary artifact. Update them first, and never let them drift.**

Any change to a behavior, a data format, the IPC protocol, the Control DB schema, an upstream
patch or a user-visible feature updates, in the same commit, the affected concept docs and the
`functions/INDEX.md` row.

Every language directory under `doc/` (currently `en-us` and `zh-cn`) mirrors the others exactly —
same files, headings, tables, and code blocks. `doc/en-us/` is authoritative: any documentation
change updates **all** language directories in the same commit, translated per
`translation-guide.md`; new terms go into its glossary. `go run ./tools/checkdocs` must pass.

**Put explanation in the docs, not here.** This file is for agent instructions only.

**`PLAN.md` is the active plan** (written in Chinese with the user) and is never copied into
`doc/`. Its design decisions change only when the user decides; record a deviation you need as a
question to the user, not as an edit. When a milestone is done, its long-lived contracts (the
protocol, the data model, the algorithms as built) move into the concept docs, and the plan's
sections for it shrink to a pointer. When the plan is complete, delete `PLAN.md`.

Add a `doc/en-us/version-history.md` entry for each release. Documentation-only commits do not
bump versions or create tags.

## Authoring rules

**Function Explanation Layer.** Every function, method and significant type carries a doc comment
(Go doc comments; `///` on C++ declarations in headers) whose first sentence states its purpose
("Observe records …"). Non-trivial declarations add short lines as needed:

- `Inputs:` important parameters only
- `Returns:` what the caller receives
- `Side effects:` state, file, network, process or UI effects
- `Notes:` assumptions, edge cases, invariants (name the `INV-xx` a check enforces)

Keep comments accurate in the same change as the code: they are the third layer of the reading
order, and a stale comment leads to a wrong change.

**Code guidelines** (shared by the MyApps projects):

- Do not over-scope: make only the changes that are requested or clearly necessary; no speculative
  features, flags or configurability, and no empty packages or directories waiting for later work.
- Do not over-abstract: no interface with a single implementation, no builders for simple objects,
  no helpers for one-time operations; fewer cohesive files beat many thin wrappers; keep
  implementation details private.
- Do not over-defend: validate at true boundaries — the Named Pipe, server responses, files and
  databases read from disk, OS callbacks — and trust internal callers.
- Use the current language versions idiomatically: the latest Go (generics where they remove
  duplication); C++ at the standard the current Nextcloud Desktop release uses, Qt idioms where Qt objects
  are involved (parent ownership, signals and slots, `QPointer` for objects that may go away).
- Name concisely; reuse existing helpers and conventions; when removing code, remove it completely.

**Upstream code.** Keep the Nextcloud Desktop snapshot (the latest release) unmodified under
`third_party/nextcloud/`;
every change to it is a small, explained patch in `patches/nextcloud/`, listed with its order and
hash in `build-lock.json`. Do not rename, reformat or reorganize upstream code. Prefer adapting in
`backend/` over patching.

**SyncD threading.** Accounts, `SyncEngine`, network access managers and the switch state machine
live on the control thread. Pipe I/O, network notifications and OS callbacks are queued to it and
carry their generation or epoch; never call a Qt object from another thread. One SQLite connection
per thread.

**Tests.** The product requires tests (`PLAN.md` §20), which overrides the MyApps default of
writing tests only on request. Tests protect product contracts and error-prone logic, preferably
table-driven; time-dependent logic (scheduling, scoring, backoff, switching) runs on a controllable
clock; every fixed bug gets a regression test. Name the invariant or §20 case a test covers. No
tests for wording, internal state details or evidence collection. An HTTP mock never stands in for
real DAV, locking or chunked-upload behavior in an acceptance claim.

Other conventions:

- MyGo UI code follows `.agents/skills/mygo-maintenance`: views are functions of state, elements
  are never stored, background results reach the view through `Window.Update`, controls get stable
  keys where state applies.
- Interface copy is written for the person using the app. Never surface error codes, operation
  IDs, epochs, hashes or internal paths in the main interface; Diagnostics may show them.
- Interface strings go through the ARB catalogs; `go tool mygo i18n check` must pass.
- The product name is **Nextcloud-MyGo** everywhere (window, tray, About, installer, shortcuts,
  release material) and its icon is the Nextcloud icon taken from the current upstream release, with the
  source path recorded in `doc/en-us/third-party.md`.

## Behavior contract

Do not change these without the user explicitly deciding to. `PLAN.md` §3 lists the invariants
(`INV-01`…`INV-15`) and §19.4 the forbidden shortcuts; both bind every change.

- **Priorities are fixed:** data integrity > credential and server-identity safety > recovery and
  endpoint switching > native ARM64 > interface > system integration. ARM64 is a first-release
  gate, not a later improvement.
- **Reuse the upstream engine.** Discovery, reconcile, propagation and the journal come from
  Nextcloud Desktop. The UI never implements WebDAV sync, and SyncD never writes its own
  replacement for them.
- **The journal is sacred.** One writer per sync root; its path and the folder's identity never
  depend on the active endpoint; errors are never "fixed" by deleting, rebuilding or hand-editing
  it, or by a full resync.
- **Credentials go only where they were allowed.** Only to an origin the user acknowledged, over
  verified TLS; never to an unknown redirect; never in IPC, logs or the Control DB. TLS checks are
  never disabled. The UI never sees an app password.
- **Reachable is not trusted.** An endpoint serves an account only after its namespace is verified;
  identity or TLS failures are hard blocks that no score overrides; trust is never inferred from
  SSIDs, matching user names or versions.
- **Switch only at a proven quiescent boundary.** A running sync cycle uses one immutable endpoint
  epoch; late events from an old epoch never move the new one. All folders of an account switch
  together.
- **Unknown is not failed, and offline is not empty.** An unconfirmed remote write is recovered by
  rediscovery; an unreachable server or unreadable folder never reads as "no files".
- **Destructive changes wait for the user** when deletion protection trips; automatic recovery
  never undoes a user's pause, accepts a changed certificate or extends the trusted address list.
- **The UI is a client.** It goes only through the IPC contract: no reading the journal, the
  Control DB or the credential store, no endpoint logic of its own. Closing the window never stops
  sync; stopping sync is an explicit call. Commands are idempotent by operation ID.
- **Native means native.** ARM64 packages contain only ARM64 binaries and DLLs, checked by tooling
  and run on real ARM64 hardware; no x64 components, no silent switch to ARM64EC.
- **Separate from the official client.** Own configuration directory, IPC names, install identity
  and update channel; never take over a folder another client manages, the official client's
  accounts, `nc://` or its file associations.
- **Default posture.** A per-user background process, never a LocalSystem service, never elevated
  at runtime; auto-update stays off until a trusted signing chain exists.
- **Never claim more than was verified;** unverified capabilities never appear as supported.

## Upstream versions

**Go, MyGo and Nextcloud are never pinned: they always follow their latest stable release.** Every
change set checks and moves to:

- the latest Go release (the `go` line in `go.mod`; `https://go.dev/dl/?mode=json`);
- the latest MyGo tag (`go list -m -versions github.com/egoist/mygo`), migrating the UI code per
  `.agents/skills/mygo-maintenance`;
- the latest Nextcloud Desktop release tag (`git ls-remote --tags https://github.com/nextcloud/desktop`)
  for the snapshot in `third_party/nextcloud/`, and the latest Nextcloud server release (plus the
  previous supported major) for the test servers.

`build-lock.json` is a record, not a pin: it states what each build actually used (tags, commits,
patch hashes, and the Qt / QtKeychain / TLS / SQLite / MSVC / Windows SDK / CMake / Ninja
combination M0 chooses). Moving Nextcloud Desktop re-applies every patch and re-runs, before
anything else, the URL, credential, quiescent-boundary, deletion-protection and
journal-compatibility checks of `PLAN.md` §16.5; a release waits until they pass, it does not fall
back to an older upstream. Add a `version-history.md` line naming the versions; record "checked,
unchanged" too.

## Verification

```bash
gofmt -l cmd internal tools
go vet ./...
go test ./...
CGO_ENABLED=0 go test ./...
go tool mygo vet .
go tool mygo i18n check
go run ./tools/checkdocs
```

- The SyncD build and test commands (x64 and ARM64) join this list, and `development.md`, with
  the first backend code. Both the upstream test build and the service build must run; "no tests
  executed" is never reported as a pass.
- Run against a real Nextcloud test instance when a change touches sync, login, endpoints or
  switching, at the latest server release; record the version used. Keys and app passwords are set by the
  user in their own terminal, never committed.
- Run the real Go ↔ C++ pipe when a change touches the protocol. Inspect the native app
  (screenshots) when a change touches layout.
- Check the architecture of every shipped binary and DLL when a change touches the build or the
  package.
- **Never claim "verified" without an actual run;** record what was verified where, on which
  machine and architecture (`doc/en-us/platform-notes.md`).

## Release, version, commit, tag, push

The version lives in `internal/version` (`Version`); `mygo.json` must match (a test enforces it),
and SyncD reports the same version. UI and SyncD are released only as one combination.

For ordinary work, do not bump versions or tag until the user confirms the release version and
confirms pushing. When the user confirms:

1. Set the version in `internal/version/version.go` and `mygo.json` (and SyncD's version source
   once it exists).
2. Bring Go, MyGo and Nextcloud to their latest releases and re-run verification.
3. Commit, including the `doc/en-us/version-history.md` entry (and its mirror).
4. Create an annotated tag `vX.Y.Z`.
5. Push **the commit first**, then the tag, to `origin` and `github`.

Signing keys and certificates live only in GitHub secrets and the maintainer's offline copy.

## Agent co-author attribution

An agent that made a real, material contribution to a commit may add its own accurate
`Co-authored-by:` trailer. Attribution is per commit: do not add an agent merely because it
reviewed, observed, or continued another agent's work, and never copy a trailer automatically from
an earlier commit. When multiple agents materially contributed, include one accurate trailer each.
Use the agent's actual documented identity; never invent a provider, model, name, or email.
Approved examples are `Co-authored-by: Codex <noreply@openai.com>` and, for Claude Code,
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` with the model name replaced by the actual
model that did the work.

## Remotes and secrets

- `origin` → `<local_gitea_address>` (private Gitea)
- `github` → `git@github.com:YuanZhe-99/Nextcloud-MyGo.git` (public)

Determine the repository path from the runtime workspace; do not hardcode a machine-specific
absolute path here.

**Masking rule:** keep the `origin` URL written as `<local_gitea_address>` in every committed file.
Never write the underlying host or port anywhere in the repo. The GitHub repository is public:
committed files never name private repositories, machines, servers, accounts or where keys are
kept. Test server addresses and user names stay out of fixtures and logs.

**Never commit:** secrets, app passwords, signing keys or certificates, `.dev/`, `build/`, backend
build trees, journals or Control DBs from test runs, or machine-specific state.

## MyApps milestone releases

For each completed milestone, increment the patch version by 0.0.1; the user decides when to
release. Documentation-only changes do not bump the version.

License: GPL-3.0 (`LICENSE`), compatible with Nextcloud Desktop's GPL-2.0-or-later code. Vendored
upstream files keep their own license headers (the snapshot carries several file-level licenses);
releases ship the license texts, notices and the corresponding source and build patches. MyGo's
MIT license is recorded separately. The Nextcloud name and icon are trademarks of Nextcloud GmbH.
