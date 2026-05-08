/**
 * @name RSA key size is below the 2048-bit minimum
 * @description Reports key-generation calls that produce RSA keys
 *   shorter than 2048 bits — the current floor for new RSA keys.
 *
 *   Detects:
 *     - `wc_MakeRsaKey(&key, N, ...)`           (wolfSSL)
 *     - `Botan::RSA_PrivateKey(rng, N)`         (Botan)
 *     - `Botan::create_private_key("RSA", rng, "N")` (string-form)
 *
 * @kind problem
 * @problem.severity error
 * @id safira/security/small-rsa-key
 * @tags safira security cryptography
 * @cwe CWE-326
 */

import cpp
import Shared

predicate isSmallIntLiteral(Expr e) {
  exists(int n | n = e.getValue().toInt() | n > 0 and n < 2048)
}

from Element e, string detail
where
  isParticipantFile(e.getFile()) and
  (
    // wolfSSL: wc_MakeRsaKey(&key, SIZE, exponent, &rng)
    exists(FunctionCall fc |
      e = fc and
      fc.getTarget().getName() = "wc_MakeRsaKey" and
      isSmallIntLiteral(fc.getArgument(1)) and
      detail = "wc_MakeRsaKey(" + fc.getArgument(1).getValue() + ")"
    )
    or
    // Botan: RSA_PrivateKey(rng, SIZE) — constructor of Botan::RSA_PrivateKey
    exists(ConstructorCall cc |
      e = cc and
      cc.getTarget().getDeclaringType().getQualifiedName() = "Botan::RSA_PrivateKey" and
      isSmallIntLiteral(cc.getArgument(1)) and
      detail = "RSA_PrivateKey(rng, " + cc.getArgument(1).getValue() + ")"
    )
    or
    // Botan: create_private_key("RSA", rng, "1024") — third arg is the param string.
    exists(FunctionCall fc, StringLiteral sizeLit |
      e = fc and
      fc.getTarget().getName() = "create_private_key" and
      fc.getArgument(0).(StringLiteral).getValue() = "RSA" and
      sizeLit = fc.getArgument(2) and
      sizeLit.getValue().regexpMatch("[0-9]+") and
      sizeLit.getValue().toInt() < 2048 and
      detail = "create_private_key(\"RSA\", _, \"" + sizeLit.getValue() + "\")"
    )
  )
select e, "RSA key size below 2048 bits: " + detail + "."
