/**
 * @name Insecure random-number source in cryptographic context
 * @description `rand()` / `srand()` and the `<random>` engines
 *   (`mt19937`, `linear_congruential_engine`, etc.) are not
 *   cryptographically secure. In TLS / X.509 code they must not be
 *   used for keys, IVs, nonces, salts, or session identifiers.
 *
 *   Use `Botan::AutoSeeded_RNG` (Botan) or `wc_InitRng` (wolfSSL)
 *   instead.
 * @kind problem
 * @problem.severity error
 * @id safira/security/insecure-rng
 * @tags safira security cryptography
 * @cwe CWE-338
 */

import cpp
import Shared

from Element e, string detail
where
  isParticipantFile(e.getFile()) and
  (
    exists(FunctionCall fc |
      e = fc and
      fc.getTarget().getName() in ["rand", "srand", "random", "drand48", "lrand48", "mrand48"] and
      detail = fc.getTarget().getName() + "()"
    )
    or
    exists(VariableDeclarationEntry vde |
      e = vde and
      vde.getType().getName().regexpMatch(".*(mt19937|minstd_rand|linear_congruential_engine|knuth_b|ranlux).*") and
      detail = vde.getType().getName()
    )
  )
select e,
  "Insecure RNG used in cryptographic code: '" + detail + "'. " +
  "Use Botan::AutoSeeded_RNG or wc_InitRng for keys / nonces / salts."
