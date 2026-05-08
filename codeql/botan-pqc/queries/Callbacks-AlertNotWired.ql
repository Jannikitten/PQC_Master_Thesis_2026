/**
 * @name TLS callback tls_alert does not surface Closed
 * @description When the peer sends close_notify the override should
 *   invoke the Closed callback (and/or set the close-notify flag) so
 *   the chat UI can react. Reports when the body never references
 *   either.
 * @kind problem
 * @problem.severity warning
 * @id safira/botan/callbacks-alert-not-wired
 * @tags safira pqc task2 callbacks
 */

import cpp
import Shared

predicate referencesClosedField(Function f) {
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and fa.getTarget().getName() = "Closed"
  )
}

predicate setsCloseNotifyFlag(Function f) {
  exists(Assignment a |
    a.getEnclosingFunction() = f and
    a.getLValue().(VariableAccess).getTarget().getName().regexpMatch("(?i).*close.?notify.*")
  )
}

from MemberFunction f
where
  f = tlsCallbackOverride("tls_alert") and
  (
    hasEmptyOrTrivialBody(f)
    or
    (not referencesClosedField(f) and not setsCloseNotifyFlag(f))
  )
select f,
  "tls_alert neither invokes the Closed callback nor records a " +
  "close_notify flag — peer disconnect will not be surfaced to the UI."
