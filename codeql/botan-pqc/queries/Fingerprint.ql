/**
 * @name BotanP2PCrypto submission fingerprint
 * @description Informational query (NOT a pass/fail check). Lists the
 *   key-exchange groups, signature schemes and protocol label that the
 *   participant configured, so the population of submissions can be
 *   analysed in aggregate.
 * @kind problem
 * @problem.severity recommendation
 * @id safira/botan/fingerprint
 * @tags safira pqc task2 fingerprint
 */

import cpp
import Shared

from MemberFunction f, string label
where
  (
    f = policyOverride("key_exchange_groups") and
    exists(EnumConstantAccess eca |
      eca.getEnclosingFunction() = f and
      label = "group:" + eca.getTarget().getName()
    )
  )
  or
  (
    f = policyOverride("allowed_signature_schemes") and
    exists(EnumConstantAccess eca |
      eca.getEnclosingFunction() = f and
      label = "signature:" + eca.getTarget().getName()
    )
  )
  or
  (
    f.getName() = "ProtocolDescription" and
    f.getDeclaringType().getName() = "BotanP2PCrypto" and
    exists(StringLiteral lit |
      lit.getEnclosingFunction() = f and
      label = "protocol:" + lit.getValue()
    )
  )
select f, label
