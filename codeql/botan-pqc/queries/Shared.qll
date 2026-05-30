/**
 * Shared helpers for the Safira Botan-PQC query pack.
 *
 * Participants may rename the scaffolding classes (e.g.
 * TLSCallbacksAdapter → TLSCallbacks). We therefore identify the
 * relevant classes structurally, by what they inherit from in Botan,
 * not by the name in the scaffolding.
 */

import cpp

/** True if some ancestor (or `c` itself) has fully-qualified name `qname`. */
private predicate hasAncestorWithQName(Class c, string qname) {
  exists(Class base | base = c.getABaseClass*() | base.getQualifiedName() = qname)
}

predicate isBotanTlsCallbacksSubclass(Class c) {
  hasAncestorWithQName(c, "Botan::TLS::Callbacks")
}

predicate isCredentialsManagerSubclass(Class c) {
  hasAncestorWithQName(c, "Botan::Credentials_Manager")
}

predicate isBotanPolicySubclass(Class c) {
  hasAncestorWithQName(c, "Botan::TLS::Policy") or
  hasAncestorWithQName(c, "Botan::TLS::Default_Policy") or
  hasAncestorWithQName(c, "Botan::TLS::Strict_Policy")
}

/**
 * True if `f` is defined inside the participant-modified directory.
 * Without this scope the override-finding helpers below match Botan's
 * own internal subclasses (Strict_Policy, Default_Policy, etc.), which
 * legitimately don't list PQ groups and would otherwise fire as false
 * positives in the participant's grading output.
 */
predicate isInParticipantCode(MemberFunction f) {
  f.getFile().getRelativePath().regexpMatch("(.*/)?Core/infrastructure/crypto/.*")
}

/**
 * The participant's TLS-callbacks override of `methodName`, regardless of
 * what they renamed their adapter class to. Scoped to participant code.
 */
MemberFunction tlsCallbackOverride(string methodName) {
  result.hasDefinition() and
  result.getName() = methodName and
  isBotanTlsCallbacksSubclass(result.getDeclaringType()) and
  isInParticipantCode(result)
}

/** The participant's policy override of `methodName`. Scoped to participant code. */
MemberFunction policyOverride(string methodName) {
  result.hasDefinition() and
  result.getName() = methodName and
  isBotanPolicySubclass(result.getDeclaringType()) and
  isInParticipantCode(result)
}

/**
 * The participant's server-side Credentials_Manager override of
 * `methodName`. Only `find_cert_chain` and `private_key_for` are
 * overridden by the server side in the scaffolding, so any
 * Credentials_Manager subclass providing those is treated as the
 * server credentials. Scoped to participant code.
 */
MemberFunction serverCredentialsOverride(string methodName) {
  result.hasDefinition() and
  result.getName() = methodName and
  isCredentialsManagerSubclass(result.getDeclaringType()) and
  methodName in ["find_cert_chain", "private_key_for"] and
  isInParticipantCode(result)
}

/** True if `name` looks like a post-quantum or PQ-hybrid key-exchange group. */
bindingset[name]
predicate pqGroupName(string name) {
  name.regexpMatch("(?i).*(ml[_ ]?kem|kyber|frodo|hqc|bike|classic[_ ]?mceliece).*")
}

/** True if `name` looks like a post-quantum signature scheme. */
bindingset[name]
predicate pqSignatureName(string name) {
  name.regexpMatch("(?i).*(ml[_ ]?dsa|dilithium|slh[_ ]?dsa|sphincs|falcon).*")
}

/**
 * True if the function body is empty / a no-op — i.e. either has no
 * statements or only a single bare `return;` / `return literal;`
 * (`{}`, `nullptr`, `0`, `""`).
 */
predicate hasEmptyOrTrivialBody(Function f) {
  f.hasDefinition() and
  exists(BlockStmt b | b = f.getBlock() |
    b.getNumStmt() = 0
    or
    (
      b.getNumStmt() = 1 and
      exists(ReturnStmt r | r = b.getStmt(0) |
        not exists(r.getExpr())
        or
        r.getExpr() instanceof Literal
        or
        r.getExpr() instanceof NullValue
      )
    )
  )
}

/** A string-literal argument inside a `throw` expression in `f`. */
string thrownStringLiteral(Function f) {
  exists(ThrowExpr te, StringLiteral lit |
    te.getEnclosingFunction() = f and
    lit.getEnclosingFunction() = f and
    lit.getParent*() = te and
    result = lit.getValue()
  )
}
