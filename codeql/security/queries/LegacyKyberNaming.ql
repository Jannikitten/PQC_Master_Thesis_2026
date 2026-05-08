/**
 * @name Pre-FIPS-203/204 algorithm name used
 * @description NIST renamed the standardised PQC primitives in
 *   August 2024: Kyber → ML-KEM (FIPS 203), Dilithium → ML-DSA
 *   (FIPS 204), SPHINCS+ → SLH-DSA (FIPS 205). The old names refer
 *   to the round-3 candidates, which differ subtly from the final
 *   standards and may not interoperate. In Botan ≥ 3.6 the old
 *   identifiers select the round-3 algorithm; mixing them with a
 *   peer that uses the standardised version silently fails to
 *   negotiate or falls back to a classical group.
 * @kind problem
 * @problem.severity warning
 * @id safira/security/legacy-pqc-naming
 * @tags safira pqc security cryptography
 */

import cpp
import Shared

bindingset[s]
predicate isLegacyName(string s) {
  s.regexpMatch("(?i).*\\bkyber[-_ ]?(512|768|1024|r3)\\b.*") or
  s.regexpMatch("(?i).*\\bdilithium[-_ ]?(2|3|5)\\b.*") or
  s.regexpMatch("(?i).*\\bsphincs[+\\-]?\\b.*")
}

bindingset[s]
predicate isCanonicalPqName(string s) {
  s.regexpMatch("(?i).*\\bml[-_ ]?kem.*") or
  s.regexpMatch("(?i).*\\bml[-_ ]?dsa.*") or
  s.regexpMatch("(?i).*\\bslh[-_ ]?dsa.*")
}

from Element e, string referenced
where
  isParticipantFile(e.getFile()) and
  (
    exists(StringLiteral lit |
      e = lit and
      isLegacyName(lit.getValue()) and
      not isCanonicalPqName(lit.getValue()) and
      referenced = lit.getValue()
    )
    or
    exists(EnumConstantAccess eca |
      e = eca and
      isLegacyName(eca.getTarget().getName()) and
      not isCanonicalPqName(eca.getTarget().getName()) and
      referenced = eca.getTarget().getName()
    )
  )
select e,
  "Pre-FIPS-203/204 PQC name '" + referenced + "' — use ML-KEM / " +
  "ML-DSA / SLH-DSA. The round-3 algorithm differs from the final " +
  "standard and may not interoperate."
