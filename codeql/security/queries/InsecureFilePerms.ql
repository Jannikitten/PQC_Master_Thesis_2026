/**
 * @name Permissive file permissions on credential file
 * @description Reports `chmod` calls whose enclosing function uses a
 *   POSIX permission macro for group / other (`S_I[RWX]GRP`,
 *   `S_I[RWX]OTH`, `S_IRWXG`, `S_IRWXO`). Identity keys must be
 *   readable only by the owner (mode 0600); wider permissions let
 *   any local user read the private key.
 *
 *   This is a within-function heuristic: it relies on the
 *   participant-modified files being small (they are). If you add
 *   chmod calls that legitimately set non-owner bits, scope this
 *   check accordingly.
 * @kind problem
 * @problem.severity error
 * @id safira/security/insecure-file-perms
 * @tags safira security filesystem
 * @cwe CWE-732
 */

import cpp
import Shared

bindingset[name]
predicate isOtherOrGroupBit(string name) {
  name in [
    "S_IRGRP", "S_IWGRP", "S_IXGRP", "S_IRWXG",
    "S_IROTH", "S_IWOTH", "S_IXOTH", "S_IRWXO"
  ]
}

from FunctionCall fc, MacroInvocation mi
where
  isParticipantFile(fc.getFile()) and
  fc.getTarget().getName() in ["chmod", "_chmod", "fchmod"] and
  locatedIn(mi, fc.getEnclosingFunction()) and
  isOtherOrGroupBit(mi.getMacroName())
select fc,
  "chmod call shares a function with permissive permission macro '" +
  mi.getMacroName() + "' — credential files must be 0600 (owner-only)."
