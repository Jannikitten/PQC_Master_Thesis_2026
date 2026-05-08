/**
 * @name ServerCredentials::find_cert_chain returns an empty chain
 * @description The server-side credentials manager must hand the TLS
 *   engine the participant's identity certificate. A body that never
 *   accesses any member field cannot be returning the stored
 *   certificate, so the handshake will fail because the server has
 *   nothing to present.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/server-creds-empty-chain
 * @tags safira pqc task2 credentials
 */

import cpp
import Shared

/**
 * The override returns the cert in some way only if it accesses one of
 * its declaring class's member fields (e.g. m_Cert, certificate_).
 */
predicate accessesAnyOwnField(MemberFunction f) {
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and
    fa.getTarget().getDeclaringType() = f.getDeclaringType()
  )
}

from MemberFunction f
where
  f = serverCredentialsOverride("find_cert_chain") and
  (hasEmptyOrTrivialBody(f) or not accessesAnyOwnField(f))
select f,
  "ServerCredentials::find_cert_chain never reads its certificate " +
  "field — server presents no certificate, handshake will fail."
