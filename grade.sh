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

restore() {
  git checkout "$SCAFFOLDING_REF" -- \
    Core/infrastructure/crypto/WolfSSLCrypto.cpp \
    Core/infrastructure/crypto/BotanP2PCrypto.cpp 2>/dev/null || true
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

  # Force the two crypto files to be re-compiled. git-checkout (the restore
  # step) bumps mtime, but after a fresh `cp` we touch explicitly so the
  # incremental build always sees them as new.
  touch Core/infrastructure/crypto/WolfSSLCrypto.cpp \
        Core/infrastructure/crypto/BotanP2PCrypto.cpp

  # Re-extract: codeql traces the (incremental) rebuild. Only the two crypto
  # files and any object files that depend on them are recompiled.
  rm -rf codeql-db
  echo "  building & extracting..."
  if codeql database create codeql-db --language=cpp \
       --command="cmake --build build -j $JOBS" \
       --overwrite 2>&1 | tee "grading/$name.build.log" \
       | grep -E '\[ *[0-9]+%\]|Linking|error:' || true; then
    :
  fi
  # Did codeql actually succeed?  Test by checking the database exists and
  # the log doesn't end in a fatal error.
  if [[ ! -d codeql-db/db-cpp ]]; then
    echo "  BUILD FAILED [$(human_time $(($(date +%s) - sub_start)))] — see grading/$name.build.log"
    restore
    continue
  fi
  rm -f "grading/$name.build.log"

  echo "  analyzing..."
  # Only run packs for tasks the participant actually attempted; otherwise
  # the unmodified scaffolding fires every task-completion query.
  analyze_packs=(codeql/security)
  for t in "${attempted[@]}"; do
    case "$t" in
      wolfssl) analyze_packs+=(codeql/wolfssl-pqc) ;;
      botan)   analyze_packs+=(codeql/botan-pqc)   ;;
    esac
  done

  # SARIF carries rule IDs (the CSV format does not), which the aggregator
  # needs to classify findings as security vs PQ vs task-completion.
  codeql database analyze codeql-db "${analyze_packs[@]}" \
    --format=sarif-latest --output="grading/$name.sarif" --rerun \
    2>/dev/null

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

# Rules that count as "PQ correctness" findings.
PQ_RULES = {
    "safira/wolfssl/configure-key-exchange-not-pq",
    "safira/wolfssl/protocol-description-not-pq",
    "safira/botan/policy-no-pq-groups",
    "safira/botan/policy-pq-kex-classical-sigs",
    "safira/security/legacy-pqc-naming",
}

# Task-pack rules that count as "security" findings (auth bypass).
SECURITY_TASKPACK_RULES = {
    "safira/botan/tofu-pin-unconditional-overwrite",
    "safira/botan/callbacks-verify-cert-chain-empty",
}

def task_for(rule, path):
    if "WolfSSLCrypto" in path or rule.startswith("safira/wolfssl/"):
        return "t1"
    if "BotanP2PCrypto" in path or rule.startswith("safira/botan/"):
        return "t2"
    return None

def category_for(rule):
    if rule in PQ_RULES:                     return "pq"
    if rule in SECURITY_TASKPACK_RULES:      return "security"
    if rule.startswith("safira/security/"):  return "security"
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
    findings_for_csv = []

    sarif_file = GRADING / f"{name}.sarif"
    build_log  = GRADING / f"{name}.build.log"
    analysis_ran = sarif_file.exists()
    build_failed = build_log.exists() and not analysis_ran

    if analysis_ran:
        for rule_id, level, uri, line, msg in parse_sarif(sarif_file):
            if not attempted_task_path(uri, attempted):
                continue
            findings_for_csv.append((rule_id, level, uri, line, msg))
            if level not in ("error", "warning"):
                continue
            task = task_for(rule_id, uri)
            cat  = category_for(rule_id)
            if task is None or cat is None:
                continue
            kind = "pq" if cat == "pq" else "sec"
            sev  = "_e" if level == "error" else "_w"
            counts[task][kind + sev] += 1

    # Write a clean per-participant CSV (replaces the codeql --format=csv output).
    with (GRADING / f"{name}.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["rule_id", "severity", "path", "line", "message"])
        for row in findings_for_csv:
            w.writerow(row)

    def status_for(task, attempted_key):
        if attempted_key not in attempted:
            return "pending"
        if build_failed:
            return "error"            # build broke — analysis couldn't run
        c = counts[task]
        return derive_status(c["pq_e"] + c["sec_e"], c["pq_w"] + c["sec_w"])

    def count_or_blank(task, attempted_key, kind):
        # When the build failed or the task wasn't attempted, the count
        # column is meaningless — leave it blank rather than reporting 0.
        if attempted_key not in attempted or build_failed:
            return ""
        if kind == "sec":
            return counts[task]["sec_e"] + counts[task]["sec_w"]
        return counts[task]["pq_e"] + counts[task]["pq_w"]

    rows.append({
        "participant": name,
        "t1_security": count_or_blank("t1", "wolfssl", "sec"),
        "t1_pq":       count_or_blank("t1", "wolfssl", "pq"),
        "t1_status":   status_for("t1", "wolfssl"),
        "t2_security": count_or_blank("t2", "botan",   "sec"),
        "t2_pq":       count_or_blank("t2", "botan",   "pq"),
        "t2_status":   status_for("t2", "botan"),
    })

with SUMMARY.open("w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=[
        "participant", "t1_security", "t1_pq", "t1_status",
        "t2_security", "t2_pq", "t2_status",
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

header = r"""\begin{table}[h]
\centering
\caption{Static analysis findings of submitted code}
\label{tab:participants}
\begin{threeparttable}
\begin{tabular}{lcccc}
\toprule
Participant & \makecell[c]{Task 1 security \\ findings \tnote{a}} & \makecell[c]{Task 1 PQ \\ correctness \tnote{b}} & \makecell[c]{Task 2 security \\ findings \tnote{a}} & \makecell[c]{Task 2 PQ \\ correctness \tnote{b}}\\
\midrule
"""

footer = r"""\bottomrule
\end{tabular}

\begin{tablenotes}
\footnotesize
\item[a] Security vulnerabilities identified via CodeQL static analysis
\item[b] Denotes the instantiation of post-quantum primitives. Absence of these schemes constituted an automatic task failure. Evaluated via CodeQL and manual review.
\end{tablenotes}
\end{threeparttable}
\end{table}
"""

def latex_escape(s):
    return str(s).replace("&", r"\&").replace("_", r"\_").replace("%", r"\%")

def cell(v):
    """Render a count: empty string (build failed / not attempted) → em-dash."""
    return r"\textemdash{}" if v == "" or v is None else str(v)

with TEX.open("w") as f:
    f.write(header)
    for r in rows:
        pid = latex_escape(r["participant"])
        f.write(f"{pid} & {cell(r['t1_security'])} & {cell(r['t1_pq'])} & "
                f"{cell(r['t2_security'])} & {cell(r['t2_pq'])} \\\\\n")
    f.write(footer)

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
