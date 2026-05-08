/**
 * @name Policy advertises a PQ key-exchange but only classical signatures
 * @description The TLS policy class returns a post-quantum or hybrid
 *   key-exchange group from key_exchange_groups(), but
 *   allowed_signature_schemes() lists only classical (RSA / ECDSA / EdDSA)
 *   schemes.
 *
 *   This is the meta-error of a PQC migration: the session keys are
 *   protected against harvest-now-decrypt-later, but the *handshake
 *   authentication* still relies on a signature primitive that a
 *   future quantum adversary can forge in real time. Effective
 *   end-to-end PQ security requires ML-DSA (Dilithium) or SLH-DSA
 *   (SPHINCS+) in addition to a PQ KEM.
 * @kind problem
 * @problem.severity warning
 * @id safira/botan/policy-pq-kex-classical-sigs
 * @tags safira pqc task2 policy security
 */

import cpp
import Shared

predicate referencesPqGroup(MemberFunction f) {
  exists(EnumConstantAccess eca |
    eca.getEnclosingFunction() = f and
    pqGroupName(eca.getTarget().getName())
  )
  or
  exists(StringLiteral lit |
    lit.getEnclosingFunction() = f and pqGroupName(lit.getValue())
  )
}

predicate referencesPqSignature(MemberFunction f) {
  exists(EnumConstantAccess eca |
    eca.getEnclosingFunction() = f and
    pqSignatureName(eca.getTarget().getName())
  )
  or
  exists(StringLiteral lit |
    lit.getEnclosingFunction() = f and pqSignatureName(lit.getValue())
  )
}

from Class policyClass, MemberFunction kex, MemberFunction sig
where
  isBotanPolicySubclass(policyClass) and
  kex = policyOverride("key_exchange_groups") and
  sig = policyOverride("allowed_signature_schemes") and
  kex.getDeclaringType() = policyClass and
  sig.getDeclaringType() = policyClass and
  referencesPqGroup(kex) and
  not referencesPqSignature(sig)
select policyClass,
  "Policy '" + policyClass.getName() + "' offers a PQ / hybrid " +
  "key-exchange but accepts only classical signature schemes — " +
  "handshake authentication is not quantum-secure. Add ML-DSA / " +
  "SLH-DSA to allowed_signature_schemes()."
