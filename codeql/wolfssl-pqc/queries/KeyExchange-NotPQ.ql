/**
 * @name WolfSSLCrypto::ConfigureKeyExchange does not select a post-quantum group
 * @description Programming Task 1 requires the DTLS handshake to use a
 *   post-quantum (or hybrid) key-exchange group. This query reports the
 *   function when its body does not reference any ML-KEM / Kyber / hybrid
 *   group constant.
 * @kind problem
 * @problem.severity error
 * @id safira/wolfssl/configure-key-exchange-not-pq
 * @tags safira pqc task1
 */

import cpp

/** Macros / identifiers that wolfSSL uses for PQ-secure key-exchange groups. */
bindingset[name]
predicate pqGroupName(string name) {
  name.regexpMatch("WOLFSSL_(ML_KEM|KYBER)_[0-9]+") or
  name.regexpMatch("WOLFSSL_P[0-9]+_(ML_KEM|KYBER)_[0-9]+") or
  name.regexpMatch("WOLFSSL_X[0-9]+_(ML_KEM|KYBER)_[0-9]+")
}

/** Holds if `e` is lexically inside the body of `f`. */
pragma[inline]
predicate locatedIn(Locatable e, Function f) {
  e.getFile() = f.getFile() and
  exists(Location body |
    body = f.getBlock().getLocation() and
    e.getLocation().getStartLine() >= body.getStartLine() and
    e.getLocation().getEndLine() <= body.getEndLine()
  )
}

/** A use of a PQ-group macro inside the function body. */
predicate referencesPqGroupMacro(Function f) {
  exists(MacroInvocation mi |
    locatedIn(mi, f) and pqGroupName(mi.getMacroName())
  )
}

/** A use of a PQ-group identifier (e.g. enum / constant) inside the function body. */
predicate referencesPqGroupIdentifier(Function f) {
  exists(VariableAccess va |
    locatedIn(va, f) and pqGroupName(va.getTarget().getName())
  )
  or
  exists(EnumConstantAccess eca |
    locatedIn(eca, f) and pqGroupName(eca.getTarget().getName())
  )
}

from MemberFunction f
where
  f.getDeclaringType().getName() = "WolfSSLCrypto" and
  f.getName() = "ConfigureKeyExchange" and
  f.hasDefinition() and
  not referencesPqGroupMacro(f) and
  not referencesPqGroupIdentifier(f)
select f,
  "ConfigureKeyExchange does not select a post-quantum key-exchange group " +
  "(expected an ML-KEM or hybrid identifier such as WOLFSSL_ML_KEM_512)."
