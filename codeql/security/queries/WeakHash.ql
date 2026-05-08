/**
 * @name Weak hash algorithm referenced
 * @description SHA-1, MD5, MD4 and MD2 are broken or deprecated.
 *   Reports any string literal or identifier in participant code that
 *   names them. Used in TLS / X.509 contexts these break authentication.
 * @kind problem
 * @problem.severity error
 * @id safira/security/weak-hash
 * @tags safira security cryptography
 * @cwe CWE-327
 */

import cpp
import Shared

bindingset[s]
predicate weakHashName(string s) {
  // Match the algorithm names in common formats: "SHA-1", "SHA1",
  // "sha1", "MD5", "Sha1", "wc_InitSha1", etc. Avoid matching SHA-256
  // / SHA-384 / SHA3-256 etc.
  s.regexpMatch("(?i).*\\b(sha[-_ ]?1|md[245])\\b.*") and
  not s.regexpMatch("(?i).*sha3.*")
}

from Element e, string referenced
where
  isParticipantFile(e.getFile()) and
  (
    exists(StringLiteral lit |
      e = lit and weakHashName(lit.getValue()) and referenced = lit.getValue()
    )
    or
    exists(FunctionCall fc |
      e = fc and weakHashName(fc.getTarget().getName()) and
      referenced = fc.getTarget().getName()
    )
  )
select e, "Weak hash algorithm referenced: '" + referenced + "'."
