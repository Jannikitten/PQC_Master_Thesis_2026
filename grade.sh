#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p grading

# Parallelism cap. macOS thrashes when -j is unbounded — defaults to 4.
# Override with: JOBS=8 ./grade.sh
JOBS="${JOBS:-4}"

human_time() {
  local s=$1
  if   (( s < 60   )); then printf '%ds'        "$s"
  elif (( s < 3600 )); then printf '%dm %ds'    "$((s/60))" "$((s%60))"
  else                      printf '%dh %dm'    "$((s/3600))" "$(((s%3600)/60))"
  fi
}

# Idempotent: on a fresh clone, install pack deps once.
echo "[setup] Installing CodeQL pack dependencies..."
for pack in codeql/wolfssl-pqc codeql/botan-pqc codeql/security; do
  (cd "$pack" && codeql pack install >/dev/null)
done

# ── One-time dependency build ───────────────────────────────────────────────
# yaml-cpp / spdlog / glm / glfw / wolfssl / botan / imgui only need to be
# compiled once. Subsequent submissions only re-compile the two crypto files,
# turning a 5-minute rebuild into ~10–20 s per submission.
if [[ ! -f build/CMakeCache.txt ]]; then
  echo "[setup] First build — compiling all dependencies (this is the slow part,"
  echo "        and only happens once). Using -j$JOBS."
  setup_start=$(date +%s)
  cmake -S . -B build
  cmake --build build -j "$JOBS"
  echo "[setup] Dependency build complete in $(human_time $(($(date +%s) - setup_start)))."
else
  echo "[setup] Reusing existing build/ (dependencies already compiled)."
fi

# Restore source: defaults to HEAD on the current branch. If your `main`
# has drifted past the canonical scaffolding (e.g. you applied the PQ fix
# yourself), set this to a clean ref like a tag or pre-fix commit, e.g.
#   SCAFFOLDING_REF=2f72bc2 ./grade.sh
SCAFFOLDING_REF="${SCAFFOLDING_REF:-HEAD}"

# ── Heal any prior bad state, then snapshot pristine scaffolding ─────────────
# Previous runs that died before their restore step (or whose `git checkout`
# raced VS Code's git extension for .git/index.lock) could leave the working
# tree holding the last participant's code. Force-restore from git up front
# so we always start from a known-good state.
echo "[setup] Restoring crypto scaffolding from $SCAFFOLDING_REF..."
if ! git checkout "$SCAFFOLDING_REF" -- \
       Core/infrastructure/crypto/WolfSSLCrypto.cpp \
       Core/infrastructure/crypto/BotanP2PCrypto.cpp; then
  cat >&2 <<EOF
[setup] FATAL: cannot restore crypto scaffolding from git ref '$SCAFFOLDING_REF'.
  Common cause: a stale .git/index.lock from VS Code's git extension.
  Workaround: pkill -9 -f 'git status' && rm -f .git/index.lock, then retry.
EOF
  exit 1
fi

# Snapshot the pristine scaffolding to a backup outside .git's reach. The
# rest of the script restores from this snapshot — no further git operations,
# no lock contention.
BACKUP_DIR="$(pwd)/.grade-backup"
mkdir -p "$BACKUP_DIR"
cp Core/infrastructure/crypto/WolfSSLCrypto.cpp  "$BACKUP_DIR/"
cp Core/infrastructure/crypto/BotanP2PCrypto.cpp "$BACKUP_DIR/"

restore() {
  cp "$BACKUP_DIR/WolfSSLCrypto.cpp"  Core/infrastructure/crypto/WolfSSLCrypto.cpp
  cp "$BACKUP_DIR/BotanP2PCrypto.cpp" Core/infrastructure/crypto/BotanP2PCrypto.cpp
}
trap restore EXIT

# Count submissions for the [i/total] progress prefix.
total=0
for sub in submissions/*/; do
  if [[ -f "$sub/WolfSSLCrypto.cpp" || -f "$sub/BotanP2PCrypto.cpp" ]]; then
    ((total++))
  fi
done
echo "[grading] $total submission(s) to process."

i=0
loop_start=$(date +%s)

for sub in submissions/*/; do
  name=$(basename "$sub")

  attempted=()
  if [[ -f "$sub/WolfSSLCrypto.cpp" ]]; then
    cp "$sub/WolfSSLCrypto.cpp" Core/infrastructure/crypto/
    attempted+=("wolfssl")
  fi
  if [[ -f "$sub/BotanP2PCrypto.cpp" ]]; then
    cp "$sub/BotanP2PCrypto.cpp" Core/infrastructure/crypto/
    attempted+=("botan")
  fi

  if [[ ${#attempted[@]} -eq 0 ]]; then
    continue
  fi

  ((i++))
  sub_start=$(date +%s)
  echo
  echo "[$i/$total] === $name ==="
  echo "  attempted: ${attempted[*]}"

  printf '%s\n' "${attempted[@]}" > "grading/$name.attempted"

  # Per-task isolated compilation: each task's crypto file is built with
  # its OWN per-source make target (no linking). A bad BotanP2PCrypto.cpp
  # can no longer kill the WolfSSLCrypto.cpp extraction, so we still get
  # data for whichever task actually compiled.
  for t in "${attempted[@]}"; do
    case "$t" in
      wolfssl)
        src="Core/infrastructure/crypto/WolfSSLCrypto.cpp"
        target="Core/infrastructure/crypto/WolfSSLCrypto.o"
        suffix="t1"
        analyze_packs=(codeql/wolfssl-pqc codeql/security)
        ;;
      botan)
        src="Core/infrastructure/crypto/BotanP2PCrypto.cpp"
        target="Core/infrastructure/crypto/BotanP2PCrypto.o"
        suffix="t2"
        analyze_packs=(codeql/botan-pqc codeql/security)
        ;;
    esac

    echo "  [$t] building & extracting..."
    touch "$src"
    rm -rf "codeql-db-$suffix"

    # Per-task compile is a single file → -j 1. Parallel make races the
    # codeql preload_tracer's pre-finalize step on macOS, which produces
    # spurious "Dataset has been finalized" errors *after* a successful
    # compile. Single-threaded extraction is deterministic.
    if codeql database create "codeql-db-$suffix" --language=cpp \
         --command="make -C build $target -j 1" \
         --overwrite >"grading/$name.$suffix.build.log" 2>&1; then
      rm -f "grading/$name.$suffix.build.log"
      codeql database analyze "codeql-db-$suffix" "${analyze_packs[@]}" \
        --format=sarif-latest --output="grading/$name.$suffix.sarif" --rerun \
        2>/dev/null
    else
      echo "    $t BUILD FAILED — see grading/$name.$suffix.build.log"
    fi
  done

  echo "  done [$(human_time $(($(date +%s) - sub_start)))]"
  restore
done

if (( i > 0 )); then
  echo
  echo "[grading] $i submission(s) processed in $(human_time $(($(date +%s) - loop_start)))."
fi

# ── Aggregate per-submission CSVs into grading/summary.csv ──────────────────
# Output columns:
#   participant, t1_security, t1_pq, t1_status,
#                t2_security, t2_pq, t2_status
#
# Counts are total findings (errors + warnings) in that category.
# Status is derived from severity composition:
#   pass    — 0 errors AND 0 warnings
#   issues  — 0 errors but ≥1 warning
#   fail    — ≥1 error
#   pending — task not attempted by this participant
echo "Aggregating findings from SARIF into grading/summary.csv + per-participant CSVs..."
python3 - <<'PYEOF'
import csv, json
from pathlib import Path

GRADING = Path("grading")
SUMMARY = GRADING / "summary.csv"

# ── Check catalogue ─────────────────────────────────────────────────────────
# Each (rule_id, category, applicable_tasks). The CSV / LaTeX legend is
# generated from this list, so adding a new query here is the only thing
# you need to change to surface it in the table.
CHECKS = [
    # Generic security checks (apply to either task, scoped by file path)
    ("safira/security/weak-hash",                      "security", ("t1", "t2")),
    ("safira/security/weak-cipher",                    "security", ("t1", "t2")),
    ("safira/security/cert-verify-disabled",           "security", ("t1", "t2")),
    ("safira/security/hardcoded-secret",               "security", ("t1", "t2")),
    ("safira/security/insecure-rng",                   "security", ("t1", "t2")),
    ("safira/security/banned-c-function",              "security", ("t1", "t2")),
    ("safira/security/insecure-file-perms",            "security", ("t1", "t2")),
    ("safira/security/small-rsa-key",                  "security", ("t1", "t2")),
    ("safira/security/secret-in-log-or-exception",     "security", ("t1", "t2")),
    ("safira/security/rsa-pkcs1-v1-5",                 "security", ("t1", "t2")),
    # Botan-only auth-bypass (T2 security)
    ("safira/botan/tofu-pin-unconditional-overwrite",      "security", ("t2",)),
    ("safira/botan/callbacks-verify-cert-chain-empty",     "security", ("t2",)),
    # PQ correctness — Task 1 (WolfSSL DTLS)
    ("safira/wolfssl/configure-key-exchange-not-pq",       "pq",       ("t1",)),
    ("safira/wolfssl/protocol-description-not-pq",         "pq",       ("t1",)),
    # PQ correctness — Task 2 (Botan TLS)
    ("safira/botan/policy-no-pq-groups",                   "pq",       ("t2",)),
    ("safira/botan/policy-pq-kex-classical-sigs",          "pq",       ("t2",)),
    # PQ correctness — cross-task (FIPS-203/204 naming)
    ("safira/security/legacy-pqc-naming",                  "pq",       ("t1", "t2")),
]

PQ_RULES              = {r for r, c, _ in CHECKS if c == "pq"}
SECURITY_RULES        = {r for r, c, _ in CHECKS if c == "security"}
SECURITY_TASKPACK_RULES = {r for r, _, _ in CHECKS
                           if r.startswith("safira/botan/") or r.startswith("safira/wolfssl/")}

def applicable_rules(task, category):
    return [r for r, c, tasks in CHECKS if c == category and task in tasks]

def task_for(rule, path):
    if "WolfSSLCrypto" in path or rule.startswith("safira/wolfssl/"):
        return "t1"
    if "BotanP2PCrypto" in path or rule.startswith("safira/botan/"):
        return "t2"
    return None

def category_for(rule):
    for r, c, _ in CHECKS:
        if r == rule:
            return c
    # Task-completion gaps (callbacks-not-wired, server-creds-empty, etc.)
    # are neither security nor PQ correctness — excluded from the table.
    return None

def derive_status(err, warn):
    if err  > 0: return "fail"
    if warn > 0: return "issues"
    return "pass"

def attempted_task_path(uri, attempted):
    """Defensive: drop findings on a file whose task wasn't attempted."""
    if "WolfSSLCrypto"  in uri and "wolfssl" not in attempted: return False
    if "BotanP2PCrypto" in uri and "botan"   not in attempted: return False
    return True

def parse_sarif(sarif_path):
    """Yield (rule_id, level, uri, line, message) for each result."""
    data = json.loads(sarif_path.read_text())
    for run in data.get("runs", []):
        # Build ruleIndex -> ruleId map (some SARIF rows reference by index only).
        rules = run.get("tool", {}).get("driver", {}).get("rules", [])
        for r in run.get("results", []):
            rule_id = r.get("ruleId")
            if not rule_id:
                idx = r.get("rule", {}).get("index", r.get("ruleIndex"))
                if isinstance(idx, int) and 0 <= idx < len(rules):
                    rule_id = rules[idx].get("id", "")
            level = r.get("level", "warning")
            msg   = (r.get("message") or {}).get("text", "")
            for loc in r.get("locations", []):
                phys = loc.get("physicalLocation", {})
                uri  = phys.get("artifactLocation", {}).get("uri", "")
                line = phys.get("region", {}).get("startLine", "")
                yield (rule_id, level, uri, line, msg)

rows = []
for attempted_file in sorted(GRADING.glob("*.attempted")):
    name = attempted_file.stem
    attempted = set(attempted_file.read_text().split())

    counts = {t: {"pq_e": 0, "pq_w": 0, "sec_e": 0, "sec_w": 0} for t in ("t1", "t2")}
    fired  = {t: set() for t in ("t1", "t2")}   # rule_ids that produced ≥1 finding per task
    findings_for_csv = []

    # Per-task SARIF / build-log detection (each task is built independently).
    sarif      = {"t1": GRADING / f"{name}.t1.sarif", "t2": GRADING / f"{name}.t2.sarif"}
    build_log  = {"t1": GRADING / f"{name}.t1.build.log", "t2": GRADING / f"{name}.t2.build.log"}
    ran        = {t: sarif[t].exists() for t in ("t1", "t2")}
    build_fail = {t: build_log[t].exists() and not ran[t] for t in ("t1", "t2")}

    for t in ("t1", "t2"):
        if not ran[t]:
            continue
        for rule_id, level, uri, line, msg in parse_sarif(sarif[t]):
            if not attempted_task_path(uri, attempted):
                continue
            findings_for_csv.append((rule_id, level, uri, line, msg))
            if level not in ("error", "warning"):
                continue
            task = task_for(rule_id, uri) or t
            cat  = category_for(rule_id)
            if cat is None:
                continue
            fired[task].add(rule_id)
            kind = "pq" if cat == "pq" else "sec"
            sev  = "_e" if level == "error" else "_w"
            counts[task][kind + sev] += 1

    # Write a clean per-participant CSV (rule_id, severity, path, line, message).
    with (GRADING / f"{name}.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["rule_id", "severity", "path", "line", "message"])
        for row in findings_for_csv:
            w.writerow(row)

    def status_for(task, attempted_key):
        if attempted_key not in attempted:
            return "pending"
        if build_fail[task]:
            return "error"
        c = counts[task]
        return derive_status(c["pq_e"] + c["sec_e"], c["pq_w"] + c["sec_w"])

    def count_or_blank(task, attempted_key, kind):
        if attempted_key not in attempted or build_fail[task]:
            return ""
        if kind == "sec":
            return counts[task]["sec_e"] + counts[task]["sec_w"]
        return counts[task]["pq_e"] + counts[task]["pq_w"]

    def passed_or_blank(task, attempted_key, category):
        if attempted_key not in attempted or build_fail[task]:
            return ""
        passed = [r for r in applicable_rules(task, category) if r not in fired[task]]
        return ";".join(passed)

    rows.append({
        "participant":        name,
        "t1_security":        count_or_blank("t1", "wolfssl", "sec"),
        "t1_security_passed": passed_or_blank("t1", "wolfssl", "security"),
        "t1_pq":              count_or_blank("t1", "wolfssl", "pq"),
        "t1_pq_passed":       passed_or_blank("t1", "wolfssl", "pq"),
        "t1_status":          status_for("t1", "wolfssl"),
        "t2_security":        count_or_blank("t2", "botan", "sec"),
        "t2_security_passed": passed_or_blank("t2", "botan", "security"),
        "t2_pq":              count_or_blank("t2", "botan", "pq"),
        "t2_pq_passed":       passed_or_blank("t2", "botan", "pq"),
        "t2_status":          status_for("t2", "botan"),
    })

with SUMMARY.open("w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=[
        "participant",
        "t1_security", "t1_security_passed", "t1_pq", "t1_pq_passed", "t1_status",
        "t2_security", "t2_security_passed", "t2_pq", "t2_pq_passed", "t2_status",
    ])
    w.writeheader()
    for r in rows:
        w.writerow(r)

print(f"  wrote {SUMMARY} ({len(rows)} participants)")
PYEOF

# ── Generate LaTeX table snippet for the thesis ─────────────────────────────
echo "Generating grading/findings_table.tex..."
python3 - <<'PYEOF'
import csv
from pathlib import Path

SUMMARY = Path("grading/summary.csv")
TEX     = Path("grading/findings_table.tex")

if not SUMMARY.exists():
    raise SystemExit("grading/summary.csv missing — nothing to write")

with SUMMARY.open() as f:
    rows = list(csv.DictReader(f))

# ── Check catalogue, with display label, hex colour, task scope. ────────────
# Same order will appear in every cell and in the legend.
SECURITY_CHECKS = [
    ("safira/security/weak-hash",                       "Weak hash",            "4E79A7", ("t1","t2")),
    ("safira/security/weak-cipher",                     "Weak cipher",          "F28E2B", ("t1","t2")),
    ("safira/security/cert-verify-disabled",            "Cert verify off",      "E15759", ("t1","t2")),
    ("safira/security/hardcoded-secret",                "Hardcoded secret",     "76B7B2", ("t1","t2")),
    ("safira/security/insecure-rng",                    "Insecure RNG",         "59A14F", ("t1","t2")),
    ("safira/security/banned-c-function",               "Banned C func.",       "B07AA1", ("t1","t2")),
    ("safira/security/insecure-file-perms",             "File perms",           "FF9DA7", ("t1","t2")),
    ("safira/security/small-rsa-key",                   "Small RSA key",        "9C755F", ("t1","t2")),
    ("safira/security/secret-in-log-or-exception",      "Secret in log",        "BC80BD", ("t1","t2")),
    ("safira/security/rsa-pkcs1-v1-5",                  r"RSA PKCS\#1 v1.5",    "8C564B", ("t1","t2")),
    ("safira/botan/tofu-pin-unconditional-overwrite",   "TOFU bypass",          "1F77B4", ("t2",)),
    ("safira/botan/callbacks-verify-cert-chain-empty",  "No cert verify",       "555555", ("t2",)),
]
PQ_CHECKS = [
    ("safira/wolfssl/configure-key-exchange-not-pq",    "PQ KEX (DTLS)",        "378d94", ("t1",)),
    ("safira/wolfssl/protocol-description-not-pq",      "PQ in label",          "77b5b6", ("t1",)),
    ("safira/botan/policy-no-pq-groups",                "PQ KEX (TLS)",         "6a408d", ("t2",)),
    ("safira/botan/policy-pq-kex-classical-sigs",       "PQ signatures",        "9671bd", ("t2",)),
    ("safira/security/legacy-pqc-naming",               "FIPS-203 naming",      "d1cbe5", ("t1","t2")),
]

def latex_escape(s):
    return str(s).replace("&", r"\&").replace("_", r"\_").replace("%", r"\%")

def circles_for(passed_field, task, checks):
    """Return LaTeX with one filled disc per passed check (in catalogue order),
    on a single line. Uses \\large bullets for visibility."""
    passed = set(passed_field.split(";")) if passed_field else set()
    out = []
    for rid, _label, color, tasks in checks:
        if task not in tasks:
            continue
        if rid in passed:
            out.append(rf"\textcolor[HTML]{{{color}}}{{\large\textbullet}}")
    return "".join(out)

def cell(passed_field, status, task, checks):
    if status == "error":
        return r"\textemdash$^{\dagger}$"
    if status == "pending":
        return r"\textemdash{}"
    rendered = circles_for(passed_field, task, checks)
    return rendered if rendered else r"\textit{none}"

# Identify build failures (used in the inline note that follows the table).
build_failed = []
for r in rows:
    failed = []
    if r["t1_status"] == "error": failed.append("Task~1")
    if r["t2_status"] == "error": failed.append("Task~2")
    if failed:
        build_failed.append(f"{r['participant']} ({', '.join(failed)})")
build_failed_note = "; ".join(build_failed) if build_failed else "none"

def legend_table(checks, n_cols=3):
    """Render a `tabular` block of n_cols `<dot> <label>` pairs.
    Bullets are \\large to match the table cells for visual consistency."""
    rows_out = []
    cur = []
    for rid, label, color, tasks in checks:
        suffix = ""
        if tasks == ("t1",):     suffix = r"~[T1 only]"
        elif tasks == ("t2",):   suffix = r"~[T2 only]"
        cur.append(rf"\textcolor[HTML]{{{color}}}{{\large\textbullet}} & {label}{suffix}")
        if len(cur) == n_cols:
            rows_out.append(" & ".join(cur) + r" \\")
            cur = []
    if cur:
        # pad incomplete row
        while len(cur) < n_cols:
            cur.append("&")
        rows_out.append(" & ".join(cur) + r" \\")
    col_spec = "@{}" + ("cl" * n_cols) + "@{}"
    body = "\n".join(rows_out)
    return f"\\begin{{tabular}}{{{col_spec}}}\n{body}\n\\end{{tabular}}"

# ─────────────────────────────────────────────────────────────────────────────
# Table itself: clean two-level header (Task 1 / Task 2 across 2 sub-columns),
# tighter row spacing, no embedded footnotes.
# ─────────────────────────────────────────────────────────────────────────────
header = r"""\begin{table}[h]
\centering
\caption{Static analysis findings on participant submissions. Each filled disc represents a CodeQL check that ran without findings on the corresponding source file; missing discs indicate checks that were violated. The colour-to-check mapping is given in the legend below the table.}
\label{tab:codeql}
\renewcommand{\arraystretch}{1.35}
\setlength{\tabcolsep}{14pt}
\begin{tabular}{l cc cc}
\toprule
& \multicolumn{2}{c}{\textbf{Task 1} (WolfSSL DTLS 1.3)}
& \multicolumn{2}{c}{\textbf{Task 2} (Botan TLS 1.3)} \\
\cmidrule(lr){2-3} \cmidrule(lr){4-5}
\textbf{Participant} & Security & PQ correctness & Security & PQ correctness \\
\midrule
"""

footer_template = r"""\bottomrule
\end{tabular}
\end{table}

\noindent
\textbf{Legend --- security checks.}\\[2pt]
__SEC_LEGEND__

\medskip
\noindent
\textbf{Legend --- PQ correctness checks.}\\[2pt]
__PQ_LEGEND__

\medskip
\noindent
\textit{Note.} \textemdash{} indicates the task was not attempted by the participant, or that their submission failed to compile and CodeQL could not analyse it; in the latter case the cell is marked with a dagger~$^{\dagger}$. Build failures: __BUILD_FAILED__. Absence of any post-quantum primitive in a participant's submission constituted an automatic task failure for the corresponding cell, regardless of the security findings.
"""

with TEX.open("w") as f:
    f.write(header)
    for r in rows:
        pid = latex_escape(r["participant"])
        f.write(
            f"{pid} & "
            f"{cell(r['t1_security_passed'], r['t1_status'], 't1', SECURITY_CHECKS)} & "
            f"{cell(r['t1_pq_passed'],       r['t1_status'], 't1', PQ_CHECKS)} & "
            f"{cell(r['t2_security_passed'], r['t2_status'], 't2', SECURITY_CHECKS)} & "
            f"{cell(r['t2_pq_passed'],       r['t2_status'], 't2', PQ_CHECKS)} \\\\\n"
        )
    f.write(footer_template
            .replace("__SEC_LEGEND__",   legend_table(SECURITY_CHECKS, n_cols=3))
            .replace("__PQ_LEGEND__",    legend_table(PQ_CHECKS,       n_cols=3))
            .replace("__BUILD_FAILED__", build_failed_note))

print(f"  wrote {TEX} ({len(rows)} rows)")
PYEOF

# ── Sync CodeQL status into the Figures workspace ───────────────────────────
# Override default with: FIGURES_DIR=/some/path ./grade.sh
FIGURES_DIR="${FIGURES_DIR:-/Users/jannikverdoner/Desktop/Thesis/Figures}"
PARTICIPANTS_CSV="$FIGURES_DIR/participants.csv"

if [[ -f "$PARTICIPANTS_CSV" ]]; then
  echo "Syncing CodeQL status into $PARTICIPANTS_CSV..."
  python3 - "$PARTICIPANTS_CSV" <<'PYEOF'
import sys, csv
from pathlib import Path

PARTICIPANTS = Path(sys.argv[1])
SUMMARY      = Path("grading/summary.csv")

if not SUMMARY.exists():
    raise SystemExit("grading/summary.csv missing — nothing to sync")

status_map = {}
with SUMMARY.open() as fh:
    for row in csv.DictReader(fh):
        status_map[row["participant"]] = (row["t1_status"], row["t2_status"])

with PARTICIPANTS.open() as fh:
    reader = csv.DictReader(fh)
    fieldnames = reader.fieldnames or []
    rows = list(reader)

required = {"id", "t1_codeql", "t2_codeql"}
missing  = required - set(fieldnames)
if missing:
    raise SystemExit(f"participants.csv missing columns: {missing}")

n_updated = 0
for r in rows:
    pid = r["id"]
    if pid in status_map:
        r["t1_codeql"], r["t2_codeql"] = status_map[pid]
        n_updated += 1

with PARTICIPANTS.open("w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=fieldnames)
    w.writeheader()
    w.writerows(rows)

print(f"  updated {n_updated} row(s) in {PARTICIPANTS}")
PYEOF
else
  echo "  skip: $PARTICIPANTS_CSV not found (set FIGURES_DIR if it lives elsewhere)"
fi

# ── Re-execute the notebook to regenerate figures ───────────────────────────
NOTEBOOK="$FIGURES_DIR/generate_figures.ipynb"

if [[ -f "$NOTEBOOK" ]]; then
  # Prefer the `jupyter` binary on PATH; fall back to `python3 -m jupyter`
  # which works when `pip install jupyter` put the binary in a user-bin
  # directory (e.g. ~/Library/Python/3.13/bin) that isn't on PATH.
  if command -v jupyter >/dev/null 2>&1; then
    JUPYTER=(jupyter)
  elif python3 -c "import nbconvert" >/dev/null 2>&1; then
    JUPYTER=(python3 -m jupyter)
  else
    JUPYTER=()
  fi

  if (( ${#JUPYTER[@]} > 0 )); then
    echo "Regenerating figures from $(basename "$NOTEBOOK") (using ${JUPYTER[*]})..."
    if (cd "$FIGURES_DIR" && "${JUPYTER[@]}" nbconvert --to notebook --execute \
          generate_figures.ipynb --output generate_figures.ipynb \
          >/dev/null 2>&1); then
      echo "  figures regenerated in $FIGURES_DIR/figures/"
    else
      echo "  WARNING: notebook execution failed — run it interactively to debug"
    fi
  else
    echo "  skip: jupyter not installed in python3 — try 'pip3 install jupyter nbconvert'"
  fi
fi

echo "Done."
