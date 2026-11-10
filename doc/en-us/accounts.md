# Accounts and login

## Ownership and storage

SyncD owns Login Flow v2 tokens in memory and stores application passwords only in Windows
Credential Manager. A durable `creating` account reserves its UUID and credential reference before
the secret is written. Only a verified user UID and DAV collection root identity can make it ready.
Interrupted creating accounts are cleaned on restart; credential cleanup failure stops startup rather
than forgetting an orphan. Neither the UI nor the Control DB receives an application password.

Reauthentication work (B04/B08/B12) uses Control schema 6 and protocol 1.5. A durable session reserves
two derived native-vault slots: candidate and previous credential. The existing account, origin consent,
folder IDs, root identities, journals and user pauses remain unchanged. A candidate must match the
original UID, account root and every mapped root before installation. Engines are gated and drained
before replacing the stable credential slot; a fresh same-endpoint epoch reloads the runtime.
Before replacement, the previous credential is backed up and `prepared` is committed. A restart in
that phase restores the previous credential; a committed `installed` phase keeps the verified new one.
Neither path guesses which secret won an interrupted write. Candidate/backup slots remain durably
reserved until cleanup succeeds. The previous app password is revoked best-effort over the approved
canonical origin; an unconfirmed revocation is an explicit warning, not silent success.
Rollback first commits `discarding` before deleting its backup, so a crash during cleanup cannot leave
`prepared` without its restoration source. A previously absent credential is restored to absence, not
invented. An identical candidate/previous password is never revoked as a superseded credential.
An unavailable local root after installation leaves its runtime blocked; it does not undo verified
credentials or force a repeated startup failure. Only credential-specific scheduler blockers may be
cleared by renewal; user pauses, TLS, root, journal and deletion-safety gates remain independent.
Pending or rolling-back renewal projects `waitingSignIn` for unpaused folders rather than retaining
stale green status. Global and individual user pauses still project as paused.
Nested Qt metadata verification retains its original login session until it returns. Cancelling and
starting a replacement session cannot destroy that in-use object or let its late result fail the new
session. A separate read-only lifetime fixture uses borrowed credentials and parent-owned native-key
cleanup, including when a regression crashes the child; it never revokes those borrowed credentials.

## Login boundary

Input defaults to HTTPS. URLs with user information, queries, fragments or ambiguous paths are
rejected. Browser and polling URLs must stay within the explicitly selected origin and installation
path. Redirects are never followed by the login/verification client; TLS verification remains enabled.
Each login session owns its network manager. Poll tokens and browser login URLs are not persisted.
Cancellation and expiry abort outstanding work and invalidate late completions.

Reauthentication metadata, live replies and credential cleanup honor the same durable account-level
Retry-After gate; alternate origins and daemon restart cannot bypass it. Every `LoginFlow` request
classifies HTTP 403, 423 and 429 with the shared metadata rules (`PERMISSION_DENIED`,
`REMOTE_LOCKED`, `SERVER_THROTTLED`), never as an authentication or identity verdict. A confirmed 429
closes the flow's request gate, then is committed through the account throttle before anything else
is sent; if that commit fails the flow sends nothing further. Retry-After keeps the shared bounds: at
least 60 seconds, at most seven days, and a longer valid deadline means no automatic retry.

A session is valid for 20 minutes of monotonic time and no server delay extends it. Polling resumes
after a delay only when that delay ends inside the session; otherwise the session fails as throttled
or locked. An expired or cancelled session never accepts a candidate. Retiring a password the flow
itself acquired is separate from session success: while the server's bounded delay holds the
revocation back, the flow stays in `discarding`, the vault slot and the durable reservation remain,
and the DELETE is sent once the delay ends, even after the session expired. A DELETE the server
refused with 429 or 423 is retried; one with an unknown outcome (transport failure, timeout) is
never replayed and ends as `APP_PASSWORD_REVOCATION_UNCONFIRMED`, as do a 403 and a delay with no
automatic retry. A daemon restart during a held revocation cannot retry it: the interrupted login or
renewal is reported as `APP_PASSWORD_REVOCATION_UNCONFIRMED` rather than as a plain expiry.
These rules are covered by synthetic transport regressions on a manual clock; real-server throttle
acceptance remains unverified.

## IPC and capability

Protocol 1.1 adds `accounts.login.status(loginSessionId)` to distinguish login-session identity from
the operation that started it. Start returns a session ID, browser URL and expiry through the usual
operation result. Status returns state and, only after account commit, account ID. A restarted daemon
expires unfinished login operations. Folder synchronization remains a separate capability.
Protocol 1.5 implements `accounts.reauthenticate(accountId, expectedRevision, operationKey)` using
the same browser session result and `accounts.login.status` polling. Waiting/rollback sessions expire
on restart without removing the account; installed replacements resume local cleanup. Completed latest
renewal status survives restart. A successful status may carry `APP_PASSWORD_REVOCATION_UNCONFIRMED`:
Go `WaitLogin` then returns the preserved account ID and `ErrCredentialCleanup` together.

## Verification scope

Native ARM64 and emulated x64 login/core contracts passed on 2026-11-05. The relocated
`tools/test-login.ps1` passed against server 35.0.2 on native ARM64: real Login Flow v2 consent,
credential storage, daemon restart persistence, confirmed server app-password revocation and owned
profile cleanup. Consent used the server's HTML forms over HTTP automation; interactive browser
automation remains unverified. Namespace checks at both approved addresses matched UID and root ID;
the alternate origin's cross-origin login URLs were rejected. No existing server files were modified.

Cancellation waits for cleanup before completing its operation receipt. Failed or cancelled flows
attempt to revoke passwords they acquired, but never revoke borrowed acceptance credentials.
Unconfirmed revocation is reported as `APP_PASSWORD_REVOCATION_UNCONFIRMED`; users can remove the
password in server security settings. Interrupted creation cleanup cannot confirm remote revocation.
Full SyncD synchronization and automatic endpoint switching remain unavailable.
