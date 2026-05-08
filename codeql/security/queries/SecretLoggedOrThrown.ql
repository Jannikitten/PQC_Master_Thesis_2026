/**
 * @name Cryptographic secret passed to a logging or exception sink
 * @description Reports calls to spdlog / printf / `<<` on streams /
 *   `throw` expressions whose argument tree contains something that
 *   looks like a secret: a `Botan::Private_Key` reference, a call to
 *   `PEM_encode` / `BER_encode`, or a variable whose name suggests
 *   key / private / secret / password.
 *
 *   Secrets that hit a log file or exception message persist beyond
 *   the program's memory and are recoverable from disk / crash dumps.
 * @kind problem
 * @problem.severity error
 * @id safira/security/secret-in-log-or-exception
 * @tags safira security cryptography
 * @cwe CWE-532 CWE-209 CWE-244
 */

import cpp
import Shared

bindingset[name]
predicate looksLikeSecretName(string name) {
  name.regexpMatch("(?i).*(privkey|priv_key|private_key|priv|secret|password|passphrase|seed|shared_secret).*") and
  // Avoid false positives on common non-secret names that happen to
  // contain "key" — fingerprint, public_key, key_type, key_path,
  // key_exchange, key_id are public values.
  not name.regexpMatch("(?i).*(public_key|pubkey|key_type|key_path|key_exchange|key_id|key_size|fingerprint).*")
}

predicate looksLikeSecretType(Type t) {
  t.toString().regexpMatch(".*(Private_Key|RSA_PrivateKey|ECDSA_PrivateKey|EC_PrivateKey|Ed25519_PrivateKey).*")
}

/** True if `e` (or any descendant) references a secret. */
predicate exprMentionsSecret(Expr e) {
  exists(Expr child | child = e.getAChild*() |
    // Calls that serialise key material.
    exists(FunctionCall fc | fc = child |
      fc.getTarget().getName() in ["PEM_encode", "BER_encode", "to_string", "raw_private_key_bits"]
      and fc.getQualifier().getType().toString().regexpMatch(".*Private_Key.*")
    )
    or
    // Variable accesses with a secret-shaped name.
    exists(VariableAccess va | va = child | looksLikeSecretName(va.getTarget().getName()))
    or
    // Field accesses with a secret-shaped name.
    exists(FieldAccess fa | fa = child | looksLikeSecretName(fa.getTarget().getName()))
    or
    // Anything typed as a Botan Private_Key.
    looksLikeSecretType(child.getType())
  )
}

predicate isLoggingSink(FunctionCall fc) {
  fc.getTarget().getQualifiedName().regexpMatch("spdlog::.*") or
  fc.getTarget().getName() in [
    "printf", "fprintf", "puts", "perror",
    "log", "info", "warn", "error", "debug", "trace", "critical"
  ]
}

predicate isStreamOutput(FunctionCall fc) {
  fc.getTarget().getName() = "operator<<"
}

from Element sink, Expr arg
where
  isParticipantFile(sink.getFile()) and
  (
    (
      exists(FunctionCall fc | fc = sink |
        (isLoggingSink(fc) or isStreamOutput(fc)) and arg = fc.getAnArgument()
      )
    )
    or
    (
      sink instanceof ThrowExpr and arg = sink.(ThrowExpr).getExpr()
    )
  ) and
  exprMentionsSecret(arg)
select sink,
  "Cryptographic secret material is passed to a logging / exception " +
  "sink — secrets must not appear in logs or error messages."
