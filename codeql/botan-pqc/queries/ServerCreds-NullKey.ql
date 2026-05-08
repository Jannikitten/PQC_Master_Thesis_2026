/**
 * @name ServerCredentials::private_key_for returns nullptr
 * @description The server-side credentials manager must return the
 *   private key matching the certificate it presents. A body that
 *   never accesses a member field cannot be returning the stored key,
 *   so the TLS engine has nothing to sign with and the handshake will
 *   fail.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/server-creds-null-key
 * @tags safira pqc task2 credentials
 */

import cpp
import Shared

predicate accessesAnyOwnField(MemberFunction f) {
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and
    fa.getTarget().getDeclaringType() = f.getDeclaringType()
  )
}

from MemberFunction f
where
  f = serverCredentialsOverride("private_key_for") and
  (hasEmptyOrTrivialBody(f) or not accessesAnyOwnField(f))
select f,
  "ServerCredentials::private_key_for never reads its key field — " +
  "TLS engine has no key to sign with, handshake will fail."
