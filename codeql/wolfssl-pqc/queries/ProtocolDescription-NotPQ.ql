/**
 * @name WolfSSLCrypto::ProtocolDescription does not advertise a PQ algorithm
 * @description The user-facing protocol label should reflect the
 *   post-quantum migration (e.g. mention "ML-KEM" or "Kyber"). Flagged when
 *   the returned string still names only a classical group like
 *   ECC-SECP256R1.
 * @kind problem
 * @problem.severity warning
 * @id safira/wolfssl/protocol-description-not-pq
 * @tags safira pqc task1
 */

import cpp

bindingset[s]
predicate mentionsPq(string s) {
  s.regexpMatch("(?i).*(ml[- ]?kem|kyber).*")
}

bindingset[s]
predicate mentionsClassicalOnly(string s) {
  s.regexpMatch("(?i).*(secp256r1|secp384r1|x25519|p-?256|p-?384).*") and
  not mentionsPq(s)
}

from MemberFunction f, StringLiteral lit
where
  f.getDeclaringType().getName() = "WolfSSLCrypto" and
  f.getName() = "ProtocolDescription" and
  f.hasDefinition() and
  lit.getEnclosingFunction() = f and
  (
    mentionsClassicalOnly(lit.getValue()) or
    not mentionsPq(lit.getValue())
  )
select f,
  "ProtocolDescription returns \"" + lit.getValue() +
  "\" — does not advertise a post-quantum algorithm."
