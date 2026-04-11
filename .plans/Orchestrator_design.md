# Orchestrator Design — 3-Agent Automated Development Loop

## Architecture Overview

```
orchestrator.sh
│
├── 1. Create git worktree at .worktrees/dev-loop (branch: dev-loop)
│
├── 2. Run PLANNER in worktree (once)
│      └── agent commits .plans/ → "planner: generate implementation plan"
│
└── 3. Loop:
       ├── Run BUILDER in worktree
       │    └── agent commits .plans/FAILURE_NOTES.md → "builder: attempt N on step M"
       │         (code changes left uncommitted)
       ├── Run EVALUATOR in worktree
       │    ├── PASS → agent commits all changes → "evaluator: step M PASS"
       │    └── FAIL → agent runs git reset --hard (discards uncommitted code)
       ├── Check stop conditions
       └── Continue or exit
```

All agents operate exclusively inside the worktree. The main working tree is never
touched. On completion the user can review the worktree branch and merge at will.

---

## Shared State Files

| File | Owner | Purpose |
|------|-------|---------|
| `.plans/PROMPT.md` | User | High-level feature description. Never modified by agents. |
| `.plans/IMPLEMENTATION_PLAN.md` | Planner creates, Evaluator mutates | Ordered step list. Steps removed from top on success. |
| `.plans/FAILURE_NOTES.md` | Builder appends, Evaluator deletes on success | Log of failed approaches for the current step. |
| `.plans/cost.json` | Orchestrator | Running token/cost accumulator. |

---

## Git Worktree Lifecycle

### Setup (orchestrator start)

```bash
WORKTREE_DIR=".worktrees/dev-loop"
BRANCH_NAME="dev-loop"

# Create branch from current HEAD if it doesn't exist
git branch "$BRANCH_NAME" HEAD 2>/dev/null || true

# Create worktree (idempotent — skip if already exists)
if [ ! -d "$WORKTREE_DIR" ]; then
  git worktree add "$WORKTREE_DIR" "$BRANCH_NAME"
fi
```

### Agent-owned commits

Each agent commits its own work before completing. The orchestrator does **not** commit
on behalf of agents.

- **Planner**: commits `.plans/IMPLEMENTATION_PLAN.md` → `"planner: generate implementation plan"`
- **Builder**: commits only `.plans/FAILURE_NOTES.md` → `"builder: attempt N on step M"`.
  Code changes are left **uncommitted** in the worktree.
- **Evaluator (PASS)**: commits all changes (code + plan updates) → `"evaluator: step M PASS"`
- **Evaluator (FAIL)**: runs `git reset --hard` to discard the builder's uncommitted code changes.
  The FAILURE_NOTES.md commit from the builder survives the reset.

### Teardown (on completion or abort)

The worktree and branch are left intact for review. The user decides whether to merge:

```bash
echo "Work is on branch '$BRANCH_NAME' in worktree '$WORKTREE_DIR'"
echo "To merge: git merge $BRANCH_NAME"
echo "To clean up: git worktree remove $WORKTREE_DIR && git branch -d $BRANCH_NAME"
```

---

## Agent Prompts

### 1. Planner (`.plans/prompts/planner.md`)

```markdown
You are a planning agent. Your sole job is to produce an implementation plan.

## Inputs
- Read `.plans/PROMPT.md` for the high-level feature description.
- Read `CLAUDE.md` for build commands, architecture, project structure, and gotchas.
- Explore the codebase to understand current state before planning.

## Output
Generate `.plans/IMPLEMENTATION_PLAN.md` with numbered steps.

Each step MUST follow this format exactly:

    ## Step N: <short title>
    **Task**: What to implement or change. Be specific about files and functions.
    **Files**: List of files to create or modify.
    **Build verification**: Command to confirm it compiles (always include cmake if new files).
    **UI test procedure**: Exact numbered sequence of mouse/keyboard actions to verify:
      1. Launch binary
      2. <specific click/type/wait actions using exact menu names and UI element labels>
      3. ...
    **Success criteria**: What the screenshot MUST show for this step to pass.
      Be concrete: "The 3D viewport displays colored splats" not "it works".

## Rules
- Steps must be small enough to implement and verify in ONE iteration.
- Steps must be independently testable via the UI.
- Each UI test procedure must be followable by an agent using xdotool —
  include exact menu paths, button labels, and expected wait times.
- Do NOT implement anything. Only plan.
- Do NOT modify any file other than `.plans/IMPLEMENTATION_PLAN.md`.

## Before completing
Commit your work:
```bash
git add .plans/IMPLEMENTATION_PLAN.md
git commit -m "planner: generate implementation plan"
```
```

### 2. Builder (`.plans/prompts/builder.md`)

```markdown
You are a builder agent. Your job is to implement the FIRST step in the
implementation plan, ensuring the project builds successfully.

## Procedure

1. Read `.plans/IMPLEMENTATION_PLAN.md` — identify the first `## Step` (this is your target).
2. Read `.plans/FAILURE_NOTES.md` if it exists. It contains previous failed approaches
   for this exact step. You MUST use a DIFFERENT approach than every one listed.
   Study WHY each approach failed and avoid the same pitfalls.
3. Read `CLAUDE.md` for build commands, architecture, and gotchas.
4. BEFORE writing any code, append your planned approach to `.plans/FAILURE_NOTES.md`:

       ### Attempt N — <brief title>
       **Approach**: <what you will do and how it differs from prior attempts>
       **Rationale**: <why this should succeed where others failed>

5. Implement the step. Follow the project's code style (.clang-format: LLVM base,
   100-col, 4-space indent, attached braces, left-aligned pointers).
6. If you added new `.cpp` files, re-run cmake configure:
       cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/local/libtorch
7. Build:
       cmake --build build -j$(nproc)
8. If the build fails, fix it. Repeat until `cmake --build` exits 0.
   Do NOT leave a broken build.

## Before completing
Commit ONLY your failure notes (do NOT commit code changes — the evaluator handles that):
```bash
git add .plans/FAILURE_NOTES.md
git commit -m "builder: attempt N on step M"
```
Replace N and M with the actual attempt and step numbers.

## Rules
- Do NOT modify `.plans/IMPLEMENTATION_PLAN.md` — that is the evaluator's job.
- Do NOT run the binary — that is the evaluator's job.
- Do NOT work on any step other than the FIRST remaining step.
- Do NOT skip the FAILURE_NOTES.md entry — it must be written BEFORE you code.
- Do NOT commit code changes — leave them uncommitted for the evaluator.
```

### 3. Evaluator (`.plans/prompts/evaluator.md`)

```markdown
You are an evaluator agent. Your job is to test whether the first step in the
implementation plan was successfully implemented by interacting with the running
application and evaluating a screenshot.

## Procedure

1. Read `.plans/IMPLEMENTATION_PLAN.md` — identify the first `## Step`, its
   **UI test procedure**, and its **Success criteria**.

2. Launch the binary in the background:
       LD_LIBRARY_PATH=$HOME/local/libtorch/lib:/usr/local/cuda/lib64 ./build/world_imagine &
       APP_PID=$!
       sleep 3

3. Follow the **UI test procedure** step by step using xdotool:
   - `xdotool search --name "World Imagine"` to find the window
   - `xdotool windowactivate --sync <WID>` to focus it
   - `xdotool mousemove --window <WID> <x> <y>` + `xdotool click 1` for clicks
   - `xdotool key <key>` for keyboard input
   - `sleep 1` between actions to let the UI respond
   - For menu items: click the menu bar location, wait, then click the item

4. After completing the test procedure, take a screenshot:
       scrot -u /tmp/eval_screenshot.png
       sleep 1

5. Kill the application:
       kill $APP_PID 2>/dev/null; wait $APP_PID 2>/dev/null

6. Evaluate the screenshot against success criteria using the Anthropic API:
       IMAGE_B64=$(base64 -w0 /tmp/eval_screenshot.png)
       CRITERIA="<paste the Success criteria from the step here>"
       RESPONSE=$(curl -s https://api.anthropic.com/v1/messages \
         -H "x-api-key: $ANTHROPIC_API_KEY" \
         -H "content-type: application/json" \
         -H "anthropic-version: 2023-06-01" \
         -d '{
           "model": "claude-sonnet-4-20250514",
           "max_tokens": 512,
           "messages": [{"role": "user", "content": [
             {"type": "image", "source": {"type": "base64", "media_type": "image/png",
              "data": "'"$IMAGE_B64"'"}},
             {"type": "text", "text": "Evaluate this screenshot against these success criteria:\n'"$CRITERIA"'\n\nRespond with exactly one line: PASS or FAIL followed by a brief reason."}
           ]}]
         }')

7. Parse the response for PASS or FAIL.

8. If PASS:
   - Delete `.plans/FAILURE_NOTES.md`
   - Remove the first `## Step` section from `.plans/IMPLEMENTATION_PLAN.md`
     and renumber remaining steps starting from 1.
   - Commit ALL changes (code + plan updates):
         git add -A
         git commit -m "evaluator: step M PASS"
   - Print: STEP_RESULT=PASS

9. If FAIL:
   - Discard the builder's uncommitted code changes:
         git reset --hard
     This preserves the builder's FAILURE_NOTES.md commit while removing
     all uncommitted code modifications.
   - Do NOT modify FAILURE_NOTES.md (builder owns that file).
   - Print: STEP_RESULT=FAIL

## Rules
- Suppress all intermediate output. Only print the final STEP_RESULT line.
- Do NOT modify source code directly. You only read the plan, run the app, and judge.
- On PASS you commit the builder's code changes along with your plan updates.
- On FAIL you discard the builder's code changes via `git reset --hard`.
```

---

## Orchestrator Script (`orchestrator.sh`)

```bash
#!/usr/bin/env bash
set -euo pipefail

# ── Configuration ──────────────────────────────────────────────
WORKTREE_DIR=".worktrees/dev-loop"
BRANCH_NAME="dev-loop"
PLANS_DIR=".plans"
PROMPTS_DIR="$PLANS_DIR/prompts"
MAX_FAILURES=10
AGENT_BUDGET=10.00    # USD per agent invocation
TOTAL_BUDGET=50.00    # USD across all invocations
COST_FILE="$PLANS_DIR/cost.json"

# ── Worktree Setup ─────────────────────────────────────────────
setup_worktree() {
  git branch "$BRANCH_NAME" HEAD 2>/dev/null || true
  if [ ! -d "$WORKTREE_DIR" ]; then
    git worktree add "$WORKTREE_DIR" "$BRANCH_NAME"
    echo "Created worktree at $WORKTREE_DIR on branch $BRANCH_NAME"
  else
    echo "Worktree already exists at $WORKTREE_DIR"
  fi
}

# ── Cost Tracking ──────────────────────────────────────────────
# Claude CLI returns cost in JSON output (total_cost_usd field).
# We accumulate across invocations in cost.json.

init_cost() {
  if [ ! -f "$WORKTREE_DIR/$COST_FILE" ]; then
    echo '{"total_usd":0.00,"invocations":[]}' \
      > "$WORKTREE_DIR/$COST_FILE"
  fi
}

update_cost() {
  local agent_name="$1"
  local result_json="$2"
  if [ -z "$result_json" ]; then return; fi

  local run_cost
  run_cost=$(echo "$result_json" | jq '.total_cost_usd // 0')

  local prev_total
  prev_total=$(jq '.total_usd' "$WORKTREE_DIR/$COST_FILE")
  local new_total
  new_total=$(echo "$prev_total + $run_cost" | bc)

  jq --arg name "$agent_name" --argjson cost "$run_cost" --argjson total "$new_total" \
    '.total_usd = $total | .invocations += [{"agent": $name, "cost_usd": $cost}]' \
    "$WORKTREE_DIR/$COST_FILE" > "$WORKTREE_DIR/$COST_FILE.tmp" \
    && mv "$WORKTREE_DIR/$COST_FILE.tmp" "$WORKTREE_DIR/$COST_FILE"

  echo "  Run cost: \$$run_cost | Total: \$$new_total"
}

check_budget() {
  local cost
  cost=$(jq '.total_usd' "$WORKTREE_DIR/$COST_FILE")
  if (( $(echo "$cost >= $TOTAL_BUDGET" | bc -l) )); then
    echo "STOP: Budget limit reached (\$$cost >= \$$TOTAL_BUDGET)"
    return 1
  fi
  return 0
}

# ── Agent Runners ──────────────────────────────────────────────
# NOTE: Agents commit their own work. The orchestrator does not commit on their behalf.
# Uses claude CLI in print mode with JSON output for cost tracking.

run_agent() {
  local name="$1"
  local prompt_file="$2"
  local result_json

  echo "── Running $name agent ──"
  result_json=$(
    cd "$WORKTREE_DIR" && \
    cat "$prompt_file" | claude -p \
      --output-format json \
      --model sonnet \
      --max-budget-usd "$AGENT_BUDGET" \
      --dangerously-skip-permissions \
      --no-session-persistence
  )

  update_cost "$name" "$result_json"
}

get_current_step() {
  grep -m1 "^## Step" "$WORKTREE_DIR/$PLANS_DIR/IMPLEMENTATION_PLAN.md" 2>/dev/null \
    | sed 's/## Step \([0-9]*\).*/\1/' || echo "0"
}

get_failure_count() {
  if [ -f "$WORKTREE_DIR/$PLANS_DIR/FAILURE_NOTES.md" ]; then
    grep -c "^### Attempt" "$WORKTREE_DIR/$PLANS_DIR/FAILURE_NOTES.md" || echo "0"
  else
    echo "0"
  fi
}

has_steps_remaining() {
  grep -q "^## Step" "$WORKTREE_DIR/$PLANS_DIR/IMPLEMENTATION_PLAN.md" 2>/dev/null
}

# ── Main ───────────────────────────────────────────────────────
main() {
  echo "=== Automated Development Loop ==="

  # Setup
  setup_worktree
  init_cost

  # Phase 1: Planner (run once if no plan exists)
  if [ ! -f "$WORKTREE_DIR/$PLANS_DIR/IMPLEMENTATION_PLAN.md" ]; then
    run_agent "planner" "$PROMPTS_DIR/planner.md"
    check_budget || { echo "Budget exceeded after planning"; exit 1; }
  else
    echo "Implementation plan already exists, skipping planner."
  fi

  # Phase 2: Builder/Evaluator loop
  local iteration=0
  while has_steps_remaining; do
    iteration=$((iteration + 1))
    local step
    step=$(get_current_step)
    local failures
    failures=$(get_failure_count)

    echo ""
    echo "=== Iteration $iteration | Step $step | Failures: $failures ==="

    # Check failure limit
    if [ "$failures" -ge "$MAX_FAILURES" ]; then
      echo "STOP: $MAX_FAILURES failures reached on step $step"
      echo "See $WORKTREE_DIR/$PLANS_DIR/FAILURE_NOTES.md for details"
      break
    fi

    # Run builder (commits FAILURE_NOTES.md, leaves code uncommitted)
    run_agent "builder" "$PROMPTS_DIR/builder.md"
    check_budget || break

    # Run evaluator (commits all on PASS, git reset --hard on FAIL)
    run_agent "evaluator" "$PROMPTS_DIR/evaluator.md"
    check_budget || break
  done

  # Summary
  echo ""
  echo "=== Loop Complete ==="
  echo "Branch: $BRANCH_NAME"
  echo "Worktree: $WORKTREE_DIR"
  local final_cost
  final_cost=$(jq '.total_usd' "$WORKTREE_DIR/$COST_FILE")
  echo "Total cost: \$$final_cost"

  if ! has_steps_remaining; then
    echo "Result: ALL STEPS COMPLETED"
  else
    echo "Result: STOPPED (check output above for reason)"
  fi

  echo ""
  echo "To review: cd $WORKTREE_DIR && git log --oneline"
  echo "To merge:  git merge $BRANCH_NAME"
  echo "To clean:  git worktree remove $WORKTREE_DIR && git branch -d $BRANCH_NAME"
}

main "$@"
```

---

## Prerequisites

```bash
# System tools
sudo apt install xdotool scrot jq bc

# Claude Code CLI (must be installed and authenticated)
# https://docs.anthropic.com/en/docs/claude-code
npm install -g @anthropic-ai/claude-code
```

`ANTHROPIC_API_KEY` must be set in the environment (used by both claude CLI and the evaluator's vision API call).

---

## Stop Conditions

| Condition | Trigger |
|-----------|---------|
| Success | No `## Step` headings remain in `IMPLEMENTATION_PLAN.md` |
| Too many failures | `FAILURE_NOTES.md` contains ≥ 10 `### Attempt` entries for the current step |
| Per-agent budget | Single invocation exceeds `--max-budget-usd $AGENT_BUDGET` ($10 default) |
| Total budget | Accumulated cost ≥ $50 across all invocations |

---

## Git History (example)

```
* evaluator: step 3 PASS          ← evaluator commits code + plan update
* builder: attempt 1 on step 3    ← builder commits FAILURE_NOTES.md only
* evaluator: step 2 PASS          ← evaluator commits code + plan update
* builder: attempt 1 on step 2    ← builder commits FAILURE_NOTES.md only
*                                  ← (attempt 2: evaluator FAIL → git reset --hard, no commit)
* builder: attempt 2 on step 1    ← builder commits FAILURE_NOTES.md only
*                                  ← (attempt 1: evaluator FAIL → git reset --hard, no commit)
* builder: attempt 1 on step 1    ← builder commits FAILURE_NOTES.md only
* planner: generate implementation plan
```

Note: failed evaluations leave no commit — the `git reset --hard` discards uncommitted
code and the evaluator simply prints `STEP_RESULT=FAIL`.

All commits live on the `dev-loop` branch inside `.worktrees/dev-loop`.
The main branch is untouched until the user explicitly merges.
