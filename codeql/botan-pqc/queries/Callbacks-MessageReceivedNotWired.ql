/**
 * @name TLS callback tls_record_received is not wired to MessageReceived
 * @description The override must surface decrypted application data to
 *   the chat layer via the MessageReceived callback. Reports when the
 *   body is empty or never references the MessageReceived field.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/callbacks-message-received-not-wired
 * @tags safira pqc task2 callbacks
 */

import cpp
import Shared

predicate referencesMessageReceivedField(Function f) {
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and fa.getTarget().getName() = "MessageReceived"
  )
}

from MemberFunction f
where
  f = tlsCallbackOverride("tls_record_received") and
  (hasEmptyOrTrivialBody(f) or not referencesMessageReceivedField(f))
select f,
  "tls_record_received does not invoke the MessageReceived callback — " +
  "incoming chat messages will be silently dropped."
