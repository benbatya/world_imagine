You are continuing work on a long-running autonomous development task. This is a FRESH context window - you have no memory of previous sessions.

@.plans/prd.json @.plans/progress.txt
run `git log --limit 10` to understand previous work

1. Find the highest-priority feature to work on and work only on that feature. This should be the one YOU decide has the highest priority - not necessarily the first in the list

2. Before changes, search codebase (don't assume not implemented).

3. Implement the requirements for the selected feature using TDD.

3. Run E2E tests via bash scripts to execute the binary and run the acceptance criteria in @.plans/prd.json

4. Update prd.json marking completed work (CAREFULLY!)

**YOU CAN ONLY MODIFY ONE FIELD: "passes"**

After thorough verification, change:
```json
"passes": false
```
to:
```json
"passes": true
```

5. Append learnings to .plans/progress.txt for future iterations.

6. Commit changes: `git commit -m "<Description of work and results>"`

ONLY WORK ON A SINGLE FEATURE PER ITERATION.

If all features complete, output <promise>COMPLETE</promise>

When you learn something new about how to run commands or patterns in the code make sure you update @CLAUDE.md using a subagent but keep it brief.

Remember: You have unlimited time across many sessions. Focus on quality over speed. Production-ready is the goal.

IMPORTANT: keep track of costs and once total cost exceeds $50, stop iterating and inform the user

