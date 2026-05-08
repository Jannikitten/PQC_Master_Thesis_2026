# Safira PQC — CodeQL grading packs

Three CodeQL query packs for automated static analysis of participant
submissions:

| Pack | Path | Purpose |
|---|---|---|
| Task 1 — WolfSSL key-exchange | [`wolfssl-pqc/`](wolfssl-pqc/) | Did Task 1 migrate the DTLS key-exchange to PQ? |
| Task 2 — Botan P2P TLS 1.3 | [`botan-pqc/`](botan-pqc/) | Did Task 2 fill every TODO with PQ-secure TLS 1.3? |
| Security hygiene | [`security/`](security/) | Common cryptographic / security antipatterns regardless of task. |

The first two packs flag **task gaps** (PQ algorithm not chosen, TODO
not filled in, callback not wired). The security pack flags
**security antipatterns** that any submission can introduce (weak hash,
disabled cert verification, hardcoded secret, banned C function, etc.).
A submission with **zero `error`-severity results** across all three
passes.

A `Fingerprint.ql` query in each task pack records the chosen
algorithms / protocol labels so the population can be analysed in
aggregate.

## Prerequisites

- [CodeQL CLI](https://github.com/github/codeql-cli-binaries/releases) on `$PATH`
- The submission must build — CodeQL extracts C++ from the compiler
  invocations.

## One-time install of pack dependencies

```bash
cd codeql/wolfssl-pqc && codeql pack install
cd ../botan-pqc      && codeql pack install
cd ../security       && codeql pack install
```

## Per-submission workflow

```bash
# 1. Build the submission so CodeQL can extract it.
SUBMISSION=/path/to/participant_NN

codeql database create "$SUBMISSION/.codeql-db" \
  --language=cpp \
  --source-root="$SUBMISSION" \
  --command="cmake -S . -B build && cmake --build build -j"

# 2. Run the WolfSSL pack.
codeql database analyze "$SUBMISSION/.codeql-db" \
  codeql/wolfssl-pqc \
  --format=csv \
  --output="$SUBMISSION/results-wolfssl.csv"

# 3. Run the Botan pack.
codeql database analyze "$SUBMISSION/.codeql-db" \
  codeql/botan-pqc \
  --format=csv \
  --output="$SUBMISSION/results-botan.csv"

# 4. Run the security pack (covers both tasks' files).
codeql database analyze "$SUBMISSION/.codeql-db" \
  codeql/security \
  --format=csv \
  --output="$SUBMISSION/results-security.csv"
```

The CSV columns are: rule name, description, severity, message, path, line.

For a SARIF report (e.g. to upload to GitHub or view in VS Code), swap
`--format=csv` for `--format=sarif-latest` and use a `.sarif` extension.

## Batch grading

```bash
for sub in submissions/*/; do
  name=$(basename "$sub")
  codeql database create "$sub/.codeql-db" --language=cpp \
    --source-root="$sub" \
    --command="cmake -S . -B build && cmake --build build -j" \
    --overwrite || { echo "$name: build failed"; continue; }

  codeql database analyze "$sub/.codeql-db" \
    codeql/wolfssl-pqc codeql/botan-pqc codeql/security \
    --format=csv --output="grading/$name.csv"
done
```

A simple per-submission scorecard then comes from counting rows by
severity in each CSV.

## Query catalogue

### `wolfssl-pqc`

| Query | Severity | Detects |
|---|---|---|
| `KeyExchange-NotPQ.ql` | error | `ConfigureKeyExchange` does not reference any ML-KEM / hybrid identifier. |
| `ProtocolDescription-NotPQ.ql` | warning | `ProtocolDescription` returned string still names a classical group. |
| `Fingerprint.ql` | recommendation | Records the chosen group and protocol label. |

### `botan-pqc`

| Query | Severity | Detects |
|---|---|---|
| `Policy-NoPQGroups.ql` | error | `key_exchange_groups()` lists no PQ / hybrid group. |
| `Policy-NoSignatureSchemes.ql` | error | `allowed_signature_schemes()` body is empty / trivial. |
| `Policy-MinVersionNotTLS13.ql` | warning | `min_version()` does not return TLS 1.3. |
| `Credentials-NotImplemented.ql` | error | `GenerateOrLoadCredentials` still throws or never produces key/cert material. |
| `ServerCreds-EmptyChain.ql` | error | `find_cert_chain` body is empty / returns `{}`. |
| `ServerCreds-NullKey.ql` | error | `private_key_for` returns `nullptr`. |
| `Callbacks-EmitDataNotWired.ql` | error | `tls_emit_data` does not invoke `EmitData`. |
| `Callbacks-MessageReceivedNotWired.ql` | error | `tls_record_received` does not invoke `MessageReceived`. |
| `Callbacks-ActivatedNotWired.ql` | error | `tls_session_activated` neither calls `Activated` nor sets the activation flag. |
| `Callbacks-AlertNotWired.ql` | warning | `tls_alert` neither calls `Closed` nor sets the close-notify flag. |
| `Callbacks-VerifyCertChainEmpty.ql` | error | `tls_verify_cert_chain` performs no identity / fingerprint check. |
| `Tofu-UnconditionalOverwrite.ql` | error | `tls_verify_cert_chain` writes to a pin store but never compares against the existing pin. |
| `Policy-PQKEXButClassicalSignatures.ql` | warning | Policy advertises a PQ key-exchange but only classical signature schemes (handshake authentication is not quantum-secure). |
| `Fingerprint.ql` | recommendation | Records groups, signature schemes, and protocol label. |

### `security`

All queries are scoped to `Core/infrastructure/crypto/` so the
framework code (and the third-party `wolfssl/` and `botan/` trees)
cannot produce noise.

| Query | Severity | Detects | CWE |
|---|---|---|---|
| `WeakHash.ql` | error | SHA-1, MD5, MD4, MD2 referenced anywhere in participant code. | CWE-327 |
| `WeakCipher.ql` | error | DES / 3DES / RC2 / RC4 / Blowfish or `ECB` mode named in a string literal. | CWE-327 |
| `CertVerifyDisabled.ql` | error | `SSL_VERIFY_NONE` macro **or** `wolfSSL_CTX_set_verify(_, 0, _)` numeric bypass. | CWE-295 |
| `HardcodedSecret.ql` | error | PEM banner, long base64, or long hex literal embedded in source. | CWE-798 |
| `InsecureRNG.ql` | error | `rand`, `srand`, `mt19937`, etc. used in participant code. | CWE-338 |
| `BannedCFunctions.ql` | warning | `gets`, `strcpy`, `strcat`, `sprintf`, `vsprintf`, `scanf`. | CWE-120 |
| `InsecureFilePerms.ql` | error | `chmod` call shares a function with a group / other permission macro. | CWE-732 |
| `SmallRSAKey.ql` | error | RSA key size below 2048 bits in `wc_MakeRsaKey`, `RSA_PrivateKey`, or `create_private_key`. | CWE-326 |
| `LegacyKyberNaming.ql` | warning | Pre-FIPS-203/204 names (`Kyber-768`, `Dilithium3`, `SPHINCS+`) — round-3 algorithms, not interoperable with the standardised versions. | CWE-1395 |
| `RSAPkcs1v15.ql` | warning | Botan TLS signature scheme `RSA_PKCS1_*` — Bleichenbacher / Marvin oracle. | CWE-327 |
| `SecretLoggedOrThrown.ql` | error | Cryptographic secret material (Private_Key, PEM_encode, secret-named variable) flowing into spdlog / streams / `throw`. | CWE-532, CWE-209 |

The Botan-side equivalent of `CertVerifyDisabled` (an empty
`tls_verify_cert_chain` override) is covered by
`botan-pqc/Callbacks-VerifyCertChainEmpty.ql` — running both packs
catches both libraries.

## Notes on robustness

- Class names in the scaffolding (e.g. `TLSCallbacksAdapter`,
  `ServerCredentials`) are matched **structurally** in
  [`Shared.qll`](botan-pqc/queries/Shared.qll): a class is recognised by
  what it inherits from (`Botan::TLS::Callbacks`,
  `Botan::Credentials_Manager`, `Botan::TLS::Default_Policy`), not by
  its name. Participants who rename the classes are still graded
  correctly.
- `Policy-MinVersionNotTLS13.ql` is informational only — `min_version`
  is not actually a virtual hook in Botan 3, so the scaffolding's
  override is dead code. The query still runs for grading
  *intent*, not effect.
- The PQ-detection regexes match `ML[_ ]KEM`, `Kyber`, `ML[_ ]DSA`,
  `Dilithium`, `SLH[_ ]DSA`, `SPHINCS`, `Falcon`, etc., so submissions
  that picked a different PQ algorithm are still recognised.
- The security pack scopes itself to `Core/infrastructure/crypto/` via
  `isParticipantFile` in [`security/queries/Shared.qll`](security/queries/Shared.qll).
  If you add other directories that participants modify (e.g.
  `Core/infrastructure/network/`), update that predicate.
