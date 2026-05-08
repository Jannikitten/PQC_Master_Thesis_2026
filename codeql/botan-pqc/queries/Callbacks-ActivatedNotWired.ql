/**
 * @name TLS callback tls_session_activated does not signal Activated
 * @description The override must call the Activated callback once the
 *   TLS handshake completes, otherwise the application never
 *   transitions to the "ready to send" state.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/callbacks-activated-not-wired
 * @tags safira pqc task2 callbacks
 */

import cpp
import Shared

predicate referencesActivatedField(Function f) {
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and fa.getTarget().getName() = "Activated"
  )
}

predicate sessionActivatedFlagSet(Function f) {
  exists(Assignment a |
    a.getEnclosingFunction() = f and
    a.getLValue().(VariableAccess).getTarget().getName().regexpMatch("(?i).*activated.*")
  )
}

from MemberFunction f
where
  f = tlsCallbackOverride("tls_session_activated") and
  (
    hasEmptyOrTrivialBody(f)
    or
    (not referencesActivatedField(f) and not sessionActivatedFlagSet(f))
  )
select f,
  "tls_session_activated does not invoke the Activated callback nor set " +
  "an internal activation flag — application will never know the " +
  "handshake completed."
