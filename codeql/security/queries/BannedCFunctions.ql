/**
 * @name Banned C string / I/O function used
 * @description Reports calls to C functions that have well-known
 *   buffer-overflow or format-string footguns and have safer
 *   alternatives:
 *
 *     gets()    — no length check, removed in C11. Use fgets().
 *     strcpy()  — no length check. Use strncpy / std::string.
 *     strcat()  — no length check. Use strncat / std::string.
 *     sprintf() — no length check. Use snprintf / std::format.
 *     vsprintf()— no length check. Use vsnprintf.
 *     scanf()   — %s with no width is unbounded. Use safer parsing.
 *
 *   The participant code is small enough that any of these is
 *   suspicious.
 * @kind problem
 * @problem.severity warning
 * @id safira/security/banned-c-function
 * @tags safira security memory-safety
 * @cwe CWE-120
 */

import cpp
import Shared

from FunctionCall fc, string banned
where
  isParticipantFile(fc.getFile()) and
  banned = fc.getTarget().getName() and
  banned in ["gets", "strcpy", "strcat", "sprintf", "vsprintf", "scanf"]
select fc, "Banned C function: '" + banned + "()'. Use a length-checked alternative."
