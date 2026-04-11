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
