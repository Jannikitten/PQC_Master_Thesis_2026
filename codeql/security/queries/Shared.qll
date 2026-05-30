/**
 * Shared helpers for the Safira security-checks query pack.
 *
 * All queries are scoped to participant-modified files via
 * `isParticipantFile`, so weak-algorithm references inside the wolfssl
 * / botan library sources do not produce noise.
 */

import cpp

/**
 * True if `f` lives in the area of the repository participants are
 * expected to modify. Currently this is the crypto directory under
 * Core/infrastructure/.
 */
predicate isParticipantFile(File f) {
  f.getRelativePath().regexpMatch("(.*/)?Core/infrastructure/crypto/.*")
}

/** True if `e` is lexically inside the body of `f`. */
pragma[inline]
predicate locatedIn(Locatable e, Function f) {
  e.getFile() = f.getFile() and
  exists(Location body |
    body = f.getBlock().getLocation() and
    e.getLocation().getStartLine() >= body.getStartLine() and
    e.getLocation().getEndLine() <= body.getEndLine()
  )
}
