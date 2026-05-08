/**
 * @name WolfSSLCrypto submission fingerprint
 * @description Informational query (NOT a pass/fail check). Records every
 *   group identifier referenced inside ConfigureKeyExchange and the
 *   protocol label returned by ProtocolDescription, so the population of
 *   submissions can be analysed without manual reading.
 * @kind problem
 * @problem.severity recommendation
 * @id safira/wolfssl/fingerprint
 * @tags safira pqc task1 fingerprint
 */

import cpp

pragma[inline]
predicate locatedIn(Locatable e, Function f) {
  e.getFile() = f.getFile() and
  exists(Location body |
    body = f.getBlock().getLocation() and
    e.getLocation().getStartLine() >= body.getStartLine() and
    e.getLocation().getEndLine() <= body.getEndLine()
  )
}

string groupChosen(MemberFunction f) {
  f.getDeclaringType().getName() = "WolfSSLCrypto" and
  f.getName() = "ConfigureKeyExchange" and
  (
    exists(MacroInvocation mi |
      locatedIn(mi, f) and
      mi.getMacroName().regexpMatch("WOLFSSL_.*") and
      result = mi.getMacroName()
    )
    or
    exists(EnumConstantAccess eca |
      locatedIn(eca, f) and result = eca.getTarget().getName()
    )
  )
}

from MemberFunction f, string label
where
  (
    f.getDeclaringType().getName() = "WolfSSLCrypto" and
    f.getName() = "ConfigureKeyExchange" and
    label = "group:" + groupChosen(f)
  )
  or
  (
    f.getDeclaringType().getName() = "WolfSSLCrypto" and
    f.getName() = "ProtocolDescription" and
    exists(StringLiteral lit |
      lit.getEnclosingFunction() = f and
      label = "protocol:" + lit.getValue()
    )
  )
select f, label
