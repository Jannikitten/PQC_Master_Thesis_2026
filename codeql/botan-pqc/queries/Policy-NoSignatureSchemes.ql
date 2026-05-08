/**
 * @name Policy::allowed_signature_schemes returns an empty list
 * @description The override must enumerate the signature schemes it will
 *   accept. An empty return value disables every signature, which makes
 *   the handshake fail and is almost certainly an unfilled TODO.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/policy-empty-signature-schemes
 * @tags safira pqc task2 policy
 */

import cpp
import Shared

from MemberFunction f
where
  f = policyOverride("allowed_signature_schemes") and
  hasEmptyOrTrivialBody(f)
select f,
  "Policy::allowed_signature_schemes has an empty / trivial body — " +
  "no signature schemes are allowed, handshake will fail."
