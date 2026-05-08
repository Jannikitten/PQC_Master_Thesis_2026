#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p grading

# Idempotent: on a fresh clone, install pack deps once.
for pack in codeql/wolfssl-pqc codeql/botan-pqc codeql/security; do
  (cd "$pack" && codeql pack install >/dev/null)
done

restore() {
  git checkout -- Core/infrastructure/crypto/WolfSSLCrypto.cpp \
                  Core/infrastructure/crypto/BotanP2PCrypto.cpp 2>/dev/null || true
}
trap restore EXIT

for sub in submissions/*/; do
  name=$(basename "$sub")
  echo "=== $name ==="

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
    echo "  no .cpp files — skipping"
    continue
  fi

  printf '%s\n' "${attempted[@]}" > "grading/$name.attempted"
  echo "  tasks attempted: ${attempted[*]}"

  rm -rf build codeql-db
  cmake -S . -B build >"grading/$name.build.log" 2>&1 || {
    echo "  CMAKE CONFIGURE FAILED — see grading/$name.build.log"
    restore
    continue
  }
  if codeql database create codeql-db --language=cpp \
       --command='cmake --build build -j' \
       --overwrite >>"grading/$name.build.log" 2>&1; then
    rm -f "grading/$name.build.log"
  else
    echo "  BUILD FAILED — see grading/$name.build.log"
    restore
    continue
  fi

  codeql database analyze codeql-db \
    codeql/wolfssl-pqc codeql/botan-pqc codeql/security \
    --format=csv --output="grading/$name.csv" --rerun

  restore
done

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
echo "Aggregating findings into grading/summary.csv..."
python3 - <<'PYEOF'
import csv
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
    if rule in PQ_RULES:
        return "pq"
    if rule in SECURITY_TASKPACK_RULES:
        return "security"
    if rule.startswith("safira/security/"):
        return "security"
    # Task-completion gaps (callbacks-not-wired, server-creds-empty, etc.)
    # are neither security nor PQ correctness — excluded from the table.
    return None

def derive_status(err_count, warn_count):
    if err_count > 0:
        return "fail"
    if warn_count > 0:
        return "issues"
    return "pass"

rows = []
for attempted_file in sorted(GRADING.glob("*.attempted")):
    name = attempted_file.stem
    attempted = set(attempted_file.read_text().split())

    counts = {
        "t1": {"pq_e": 0, "pq_w": 0, "sec_e": 0, "sec_w": 0},
        "t2": {"pq_e": 0, "pq_w": 0, "sec_e": 0, "sec_w": 0},
    }

    csv_file = GRADING / f"{name}.csv"
    if csv_file.exists():
        with csv_file.open() as fh:
            for row in csv.reader(fh):
                if len(row) < 5:
                    continue
                rule, _desc, severity, _msg, path = row[0], row[1], row[2], row[3], row[4]
                if severity not in ("error", "warning"):
                    continue
                task = task_for(rule, path)
                cat = category_for(rule)
                if task is None or cat is None:
                    continue
                kind = "pq" if cat == "pq" else "sec"
                sev = "_e" if severity == "error" else "_w"
                counts[task][kind + sev] += 1

    def status_for(task_key, attempted_key):
        if attempted_key not in attempted:
            return "pending"
        c = counts[task_key]
        return derive_status(c["pq_e"] + c["sec_e"], c["pq_w"] + c["sec_w"])

    rows.append({
        "participant": name,
        "t1_security": counts["t1"]["sec_e"] + counts["t1"]["sec_w"],
        "t1_pq":       counts["t1"]["pq_e"]  + counts["t1"]["pq_w"],
        "t1_status":   status_for("t1", "wolfssl"),
        "t2_security": counts["t2"]["sec_e"] + counts["t2"]["sec_w"],
        "t2_pq":       counts["t2"]["pq_e"]  + counts["t2"]["pq_w"],
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

with TEX.open("w") as f:
    f.write(header)
    for r in rows:
        pid = latex_escape(r["participant"])
        f.write(f"{pid} & {r['t1_security']} & {r['t1_pq']} & {r['t2_security']} & {r['t2_pq']} \\\\\n")
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
  if command -v jupyter >/dev/null 2>&1; then
    echo "Regenerating figures from $(basename "$NOTEBOOK")..."
    if (cd "$FIGURES_DIR" && jupyter nbconvert --to notebook --execute \
          generate_figures.ipynb --output generate_figures.ipynb \
          >/dev/null 2>&1); then
      echo "  figures regenerated in $FIGURES_DIR/figures/"
    else
      echo "  WARNING: notebook execution failed — run it interactively to debug"
    fi
  else
    echo "  skip: jupyter not on PATH — install with 'pip install jupyter' to auto-regenerate figures"
  fi
fi

echo "Done."
