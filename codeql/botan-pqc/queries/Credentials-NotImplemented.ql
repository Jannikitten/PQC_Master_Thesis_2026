/**
 * @name GenerateOrLoadCredentials is unimplemented
 * @description Reports the credential-generation function when it still
 *   throws the scaffolding's "not implemented" runtime_error or when
 *   its body is empty / trivial — both indicate that Task 2 step (2)
 *   was not completed.
 *
 *   The check accepts either name (`GenerateOrLoadCredentials` or
 *   `LoadOrGenerateCredentials`) since the reference solution reorders
 *   the verb.
 * @kind problem
 * @problem.severity error
 * @id safira/botan/credentials-not-implemented
 * @tags safira pqc task2 credentials
 */

import cpp
import Shared

predicate stillThrowsNotImplemented(Function f) {
  exists(string s | s = thrownStringLiteral(f) |
    s.regexpMatch("(?i).*not[ _-]?implemented.*")
  )
}

from Function f
where
  f.hasDefinition() and
  f.getName().regexpMatch("(?i)(Generate|Load)Or(Generate|Load)Credentials") and
  (
    stillThrowsNotImplemented(f) or
    hasEmptyOrTrivialBody(f)
  )
select f,
  "GenerateOrLoadCredentials is still the scaffolding stub — Task 2 " +
  "step (2) is incomplete."
