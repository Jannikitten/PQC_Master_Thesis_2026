/**
 * @name TOFU pin store written without first comparing to existing pin
 * @description The tls_verify_cert_chain override writes to a
 *   pin / fingerprint store but performs no equality comparison
 *   anywhere in its body. This means the override silently accepts
 *   any new fingerprint instead of detecting a mismatch — defeating
 *   the entire point of trust-on-first-use pinning.
 *
 *   This is a *project-specific* check for Programming Task 2: a
 *   correct override must (a) read the existing pin, (b) compare it
 *   to the presented certificate's fingerprint, and (c) only insert
 *   a new pin when the slot is empty / on first use.
 *
 *   The check is heuristic: if the body never invokes a
 *   pin / fingerprint helper that performs the compare for it, AND
 *   never uses `==` / `!=` / `find` / `at` / `contains` /
 *   `count`, AND does write to a map-shaped value, we flag.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/tofu-pin-unconditional-overwrite
 * @tags safira pqc task2 callbacks security
 * @cwe CWE-295
 */

import cpp
import Shared

predicate writesPinStore(MemberFunction f) {
  // Calls to map-mutation methods on a container with a map-shaped type.
  exists(FunctionCall fc | fc.getEnclosingFunction() = f |
    fc.getTarget().getName() in [
      "insert_or_assign", "insert", "emplace", "emplace_hint",
      "put", "set", "store", "push_back"
    ] and
    fc.getQualifier().getType().toString().regexpMatch(".*(map|unordered_map|dictionary|store).*")
  )
  or
  // Assignment via operator[] on a map-shaped object.
  exists(Assignment a | a.getEnclosingFunction() = f |
    exists(FunctionCall sub | sub = a.getLValue() and sub.getTarget().getName() = "operator[]")
  )
  or
  // Direct call to a "VerifyOrTrust" / "trust" / "save" helper that itself
  // mutates the store — we don't recurse, but if the participant's body
  // delegates everything we fall through to readsPinStore below.
  exists(FunctionCall fc | fc.getEnclosingFunction() = f |
    fc.getTarget().getName().regexpMatch("(?i).*(save|persist|write|flush)pin.*")
  )
}

predicate readsPinStore(MemberFunction f) {
  // Equality / inequality on any expression — covers fingerprint compare.
  exists(BinaryOperation bo | bo.getEnclosingFunction() = f and bo.getOperator() in ["==", "!="])
  or
  // Lookup-style methods on a container.
  exists(FunctionCall fc | fc.getEnclosingFunction() = f |
    fc.getTarget().getName() in ["find", "contains", "count", "at"]
  )
  or
  // Delegation to a TOFU helper whose body reads the store.
  exists(FunctionCall fc | fc.getEnclosingFunction() = f |
    fc.getTarget()
      .getName()
      .regexpMatch("(?i).*(verify|check|match|trust|tofu|compare).*(pin|fingerprint|fp).*")
  )
  or
  // Or the helper delegated to has the secret-name on the other side.
  exists(FunctionCall fc | fc.getEnclosingFunction() = f |
    fc.getTarget()
      .getName()
      .regexpMatch("(?i).*(pin|fingerprint|fp).*(verify|check|match|trust|tofu|compare).*")
  )
}

from MemberFunction f
where
  f = tlsCallbackOverride("tls_verify_cert_chain") and
  writesPinStore(f) and
  not readsPinStore(f)
select f,
  "tls_verify_cert_chain writes to a pin / fingerprint store with no " +
  "comparison or lookup against the existing pin — TOFU policy is " +
  "bypassed and any peer certificate will be silently trusted."
