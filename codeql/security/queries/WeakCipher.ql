/**
 * @name Weak / broken cipher referenced
 * @description DES, 3DES, RC2, RC4, Blowfish, and ECB-mode block
 *   ciphers are insecure for confidentiality. Reports any string
 *   literal in participant code that names them.
 * @kind problem
 * @problem.severity error
 * @id safira/security/weak-cipher
 * @tags safira security cryptography
 * @cwe CWE-327
 */

import cpp
import Shared

bindingset[s]
predicate weakCipherName(string s) {
  // Whole-word DES / 3DES / RC2 / RC4 / Blowfish, or any mention of
  // ECB mode (e.g. "AES-128/ECB", "ECB(AES-128)").
  s.regexpMatch("(?i).*\\b(3des|tdes|des|rc[24]|blowfish)\\b.*") or
  s.regexpMatch("(?i).*\\becb\\b.*")
}

from StringLiteral lit
where
  isParticipantFile(lit.getFile()) and
  weakCipherName(lit.getValue())
select lit, "Weak cipher referenced: '" + lit.getValue() + "'."
