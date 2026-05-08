/**
 * @name Policy::key_exchange_groups does not return a post-quantum group
 * @description The TLS policy must select at least one PQ or PQ-hybrid
 *   group (e.g. ML_KEM_768, HYBRID_X25519_ML_KEM_768) to defeat
 *   harvest-now-decrypt-later attacks. Reports the override when no PQ
 *   group identifier appears in its body.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/policy-no-pq-groups
 * @tags safira pqc task2 policy
 */

import cpp
import Shared

predicate referencesPqGroup(MemberFunction f) {
  exists(EnumConstantAccess eca |
    eca.getEnclosingFunction() = f and
    pqGroupName(eca.getTarget().getName())
  )
  or
  // Botan also lets users construct Group_Params from a string ID.
  exists(StringLiteral lit |
    lit.getEnclosingFunction() = f and pqGroupName(lit.getValue())
  )
}

from MemberFunction f
where
  f = policyOverride("key_exchange_groups") and
  not referencesPqGroup(f)
select f,
  "Policy::key_exchange_groups does not reference any PQ-secure group " +
  "(expected ML_KEM_* or HYBRID_*ML_KEM_*)."
