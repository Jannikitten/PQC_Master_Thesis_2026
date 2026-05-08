/**
 * @name Hardcoded cryptographic material in source
 * @description Reports string literals that look like PEM-encoded keys
 *   or certificates, long base64 blobs, or long hex strings —
 *   suggesting a private key or symmetric secret was embedded directly
 *   in the source.
 * @kind problem
 * @problem.severity error
 * @id safira/security/hardcoded-secret
 * @tags safira security cryptography
 * @cwe CWE-798
 */

import cpp
import Shared

bindingset[v]
predicate looksLikeSecret(string v) {
  // PEM banner — strongest signal.
  v.regexpMatch("(?s).*-----BEGIN (RSA |EC |DSA |OPENSSH |PGP |PRIVATE|ENCRYPTED |CERTIFICATE)?(PRIVATE KEY|CERTIFICATE).*-----.*")
  or
  // Long contiguous base64 (>= 96 chars, no whitespace mid-token).
  (
    v.length() >= 96 and
    v.regexpMatch("[A-Za-z0-9+/=]+") and
    // Avoid catching things like long URL paths.
    v.regexpMatch(".*[A-Z].*") and v.regexpMatch(".*[a-z].*") and v.regexpMatch(".*[0-9].*")
  )
  or
  // Long hex (>= 64 chars).
  (v.length() >= 64 and v.regexpMatch("[0-9A-Fa-f]+"))
}

from StringLiteral lit
where
  isParticipantFile(lit.getFile()) and
  looksLikeSecret(lit.getValue())
select lit, "String literal looks like embedded cryptographic material — " +
  "secrets must not be hardcoded in source."
