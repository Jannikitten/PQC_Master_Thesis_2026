/**
 * @name TLS callback tls_emit_data is not wired to EmitData
 * @description The override must forward the encrypted bytes Botan
 *   produces to the application via the EmitData callback. Reports
 *   when the body is empty or never references the EmitData field.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/callbacks-emit-data-not-wired
 * @tags safira pqc task2 callbacks
 */

import cpp
import Shared

predicate referencesEmitDataField(Function f) {
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and fa.getTarget().getName() = "EmitData"
  )
}

from MemberFunction f
where
  f = tlsCallbackOverride("tls_emit_data") and
  (hasEmptyOrTrivialBody(f) or not referencesEmitDataField(f))
select f,
  "tls_emit_data does not invoke the EmitData callback — encrypted " +
  "bytes will never reach the socket."
