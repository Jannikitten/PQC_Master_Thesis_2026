/**
 * @name Policy::min_version is not pinned to TLS 1.3
 * @description Programming Task 2 explicitly targets TLS 1.3.
 *   The min_version override must return Protocol_Version::TLS_V13.
 *
 *   NOTE: `min_version` is *not* a virtual hook in Botan 3's
 *   Botan::TLS::Policy, so this method is dead code in the scaffolding.
 *   The query is still useful for grading — it documents the
 *   participant's intent and catches missing/empty bodies.
 * @kind problem
 * @problem.severity warning
 * @id safira/botan/policy-min-version-not-tls13
 * @tags safira pqc task2 policy
 */

import cpp
import Shared

predicate returnsTls13(MemberFunction f) {
  exists(EnumConstantAccess eca |
    eca.getEnclosingFunction() = f and
    eca.getTarget().getName().regexpMatch("TLS_V?13|TLS_V_1_3|TLSv1_3")
  )
  or
  // Constructed via Protocol_Version(13) or similar literal.
  exists(Literal l |
    l.getEnclosingFunction() = f and
    l.getValueText() = "13"
  )
}

from MemberFunction f
where
  f.hasDefinition() and
  f.getName() = "min_version" and
  isBotanPolicySubclass(f.getDeclaringType()) and
  not returnsTls13(f)
select f,
  "Policy::min_version does not return Protocol_Version::TLS_V13."
