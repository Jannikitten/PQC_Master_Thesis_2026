/**
 * @name Certificate verification disabled
 * @description Reports calls that explicitly disable peer certificate
 *   verification:
 *
 *     - The `SSL_VERIFY_NONE` macro (wolfSSL / OpenSSL).
 *     - A literal `0` passed as the `mode` argument to
 *       wolfSSL_CTX_set_verify / wolfSSL_set_verify (since
 *       `SSL_VERIFY_NONE == 0`, this slips past the macro check).
 *
 *   Disabling verification breaks authentication and enables trivial
 *   MITM attacks. The Botan-side equivalent (an empty
 *   tls_verify_cert_chain override) is covered by the botan-pqc pack.
 * @kind problem
 * @problem.severity error
 * @id safira/security/cert-verify-disabled
 * @tags safira security tls
 * @cwe CWE-295
 */

import cpp
import Shared

from Element e, string detail
where
  isParticipantFile(e.getFile()) and
  (
    exists(MacroInvocation mi |
      e = mi and
      mi.getMacroName() = "SSL_VERIFY_NONE" and
      detail = "SSL_VERIFY_NONE"
    )
    or
    exists(FunctionCall fc, Expr modeArg |
      e = fc and
      fc.getTarget().getName() in [
        "wolfSSL_CTX_set_verify", "wolfSSL_set_verify",
        "SSL_CTX_set_verify",    "SSL_set_verify"
      ] and
      modeArg = fc.getArgument(1) and
      modeArg.getValue() = "0" and
      detail = fc.getTarget().getName() + "(_, 0, _)"
    )
  )
select e,
  "Certificate verification disabled: '" + detail + "' — handshake " +
  "becomes vulnerable to MITM."
