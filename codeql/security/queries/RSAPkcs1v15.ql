/**
 * @name RSA PKCS#1 v1.5 signature scheme used
 * @description PKCS#1 v1.5 RSA signatures are vulnerable to
 *   Bleichenbacher / Marvin-style timing oracles (CVE-2023-50782 and
 *   the 2024 follow-ups across multiple TLS stacks). New code should
 *   use RSA-PSS, Ed25519, or — for thesis-quantum-secure code —
 *   ML-DSA (Dilithium).
 *
 *   This query flags the Botan TLS Signature_Scheme enum constants
 *   `RSA_PKCS1_*` only. The wolfSSL scaffolding's existing
 *   `CTC_SHA256wRSA` certificate signature is intentionally left
 *   un-flagged because participants do not edit that line; if you
 *   want to grade on cert-signature too, broaden the regex below.
 * @kind problem
 * @problem.severity warning
 * @id safira/security/rsa-pkcs1-v1-5
 * @tags safira security cryptography
 * @cwe CWE-327
 */

import cpp
import Shared

from EnumConstantAccess eca
where
  isParticipantFile(eca.getFile()) and
  eca.getTarget().getName().regexpMatch("RSA_PKCS1(_SHA[0-9]+)?")
select eca,
  "RSA PKCS#1 v1.5 signature scheme '" + eca.getTarget().getName() +
  "' — use RSA-PSS, Ed25519, or ML-DSA instead (Bleichenbacher / Marvin)."
