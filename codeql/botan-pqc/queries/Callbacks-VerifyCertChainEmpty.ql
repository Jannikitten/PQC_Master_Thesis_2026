/**
 * @name TLS callback tls_verify_cert_chain performs no verification
 * @description The override is the only place the participant can
 *   enforce TOFU pinning / identity matching. An empty body silently
 *   accepts any certificate the peer presents, defeating
 *   authentication.
 *
 *   The query also flags overrides that *only* call the parent
 *   implementation, since the parent has no fingerprint store.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/callbacks-verify-cert-chain-empty
 * @tags safira pqc task2 callbacks security
 */

import cpp
import Shared

predicate doesAnyVerification(Function f) {
  // Calls something whose name suggests fingerprint / identity / pin /
  // verify / TOFU work, OR reads a fingerprint from the chain.
  exists(FunctionCall fc | fc.getEnclosingFunction() = f |
    fc.getTarget()
      .getName()
      .regexpMatch("(?i).*(fingerprint|verify|pin|trust|tofu|" +
                   "subject_dn|get_first_attribute).*")
  )
  or
  // References a member named like a TOFU helper / store.
  exists(VariableAccess va | va.getEnclosingFunction() = f |
    va.getTarget().getName().regexpMatch("(?i).*(fingerprint|pin|tofu).*")
  )
}

from MemberFunction f
where
  f = tlsCallbackOverride("tls_verify_cert_chain") and
  (hasEmptyOrTrivialBody(f) or not doesAnyVerification(f))
select f,
  "tls_verify_cert_chain performs no identity / fingerprint check — " +
  "any peer certificate will be accepted."
