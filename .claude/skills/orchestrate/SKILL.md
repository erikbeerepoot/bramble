---
name: orchestrate
description: Decompose complex tasks into sub-tasks, delegate to sub-agents, track progress, and synthesize results.
---

# General-Purpose Task Orchestrator

Decompose complex tasks into sub-tasks, delegate to sub-agents, track progress, and synthesize results.

## Arguments

- `$ARGUMENTS` — Task description (natural language or numbered list), OR one of:
  - `--dry-run` flag after description — decompose and show plan only, do not execute
  - `--sequential` flag after description — force one-at-a-time execution (no parallel dispatch)
  - `continue` or `resume` — resume from existing TaskList state

## Overview

```
/orchestrate "<task description>"
    │
    ▼
Phase 1: PLAN — Analyze and decompose into sub-tasks
    │
    ▼
Phase 2: TRACK — Create tasks with dependencies
    │
    ▼
Phase 3: EXECUTE — Dispatch sub-agents, track progress
    │
    ▼
Phase 4: SYNTHESIZE — Collect results and report
```

---

## Phase 1: PLAN — Analyze and Decompose

### If `$ARGUMENTS` is `continue` or `resume`:

Skip to **Phase 3: EXECUTE (Resumption)**.

### If `$ARGUMENTS` is a numbered list:

Parse directly into sub-tasks. Each numbered item becomes one sub-task. Infer types and dependencies from the text.

### If `$ARGUMENTS` is natural language:

1. **Analyze the task** — Determine if codebase exploration is needed:
   - If the task references specific files, modules, or code constructs: launch an **Explore sub-agent** via the Task tool (`subagent_type: Explore`) to understand the relevant code structure, patterns, and dependencies.
   - If the task is self-contained (e.g., "run tests then commit"): skip exploration.

2. **Decompose into sub-tasks** — Break the task into the smallest independently-completable units of work. For each sub-task, determine:

   | Field | Description |
   |-------|-------------|
   | `id` | Sequential number (1, 2, 3...) |
   | `title` | Short imperative description (e.g., "Extract color logic from NeoPixel HAL") |
   | `type` | One of: `explore`, `implement`, `command`, `skill:<name>` |
   | `depends_on` | List of sub-task IDs that must complete first (empty = independent) |
   | `description` | Detailed instructions for the sub-agent, including file paths, acceptance criteria |
   | `activeForm` | Present-continuous form (e.g., "Extracting color logic") |

3. **Classify sub-task types** using this table:

   | Type | When to use | Dispatched as |
   |------|-------------|---------------|
   | `explore` | Reading code, searching, answering questions | Task tool with `subagent_type: Explore` |
   | `implement` | Writing/editing code, refactoring, fixing bugs | Task tool with `subagent_type: general-purpose` |
   | `command` | Running shell commands (build, test, git) | Bash tool directly |
   | `skill:<name>` | Invoking another skill (commit, bramble-build, etc.) | Skill tool with `skill: <name>` |

4. **Build the dependency graph** — Independent tasks have no `depends_on`. Tasks that require output from earlier tasks list those IDs. Maximize parallelism: only add dependencies where the output of one task is genuinely needed as input to another.

### Output of Phase 1

Present the decomposition to the user as a numbered list:

```
Orchestration Plan for: "<original task>"

Sub-tasks:
  1. [explore] Analyze NeoPixel HAL structure
  2. [implement] Extract color utility functions → new file (depends on: 1)
  3. [implement] Update NeoPixel HAL to use extracted functions (depends on: 1, 2)
  4. [skill:bramble-build] Build SENSOR variant (depends on: 2, 3)
  5. [skill:commit] Commit changes (depends on: 4)

Parallel groups:
  - Group A (independent): [1]
  - Group B (after 1): [2, 3] — run in parallel
  - Group C (after 2,3): [4]
  - Group D (after 4): [5]
```

**If `--dry-run` is present:** Stop here. Display the plan and exit.

**Otherwise:** Ask the user for confirmation before proceeding using `AskUserQuestion`:
- "Proceed with this plan?"
- Options: "Yes, execute", "Modify plan", "Cancel"

If the user selects "Modify plan", ask what changes they want and re-decompose.

---

## Phase 2: TRACK — Create and Wire Tasks

After user confirmation:

1. **Create tasks** — For each sub-task, call `TaskCreate` with:
   - `subject`: The sub-task title
   - `description`: The detailed description from Phase 1
   - `activeForm`: The present-continuous form

2. **Wire dependencies** — For each sub-task that has `depends_on`, call `TaskUpdate` with:
   - `taskId`: The created task's ID
   - `addBlockedBy`: Array of task IDs corresponding to the `depends_on` sub-task IDs

   Map Phase 1 sub-task IDs (1, 2, 3...) to the actual TaskCreate IDs returned by the system.

3. **Verify** — Call `TaskList` to confirm all tasks are created with correct dependency wiring.

---

## Phase 3: EXECUTE — Dispatch and Track

### Normal Execution

1. **Find ready tasks** — Call `TaskList`. Identify all tasks with status `pending` and empty `blockedBy` (no unresolved dependencies).

2. **Dispatch ready tasks** — For each ready task, based on its type:

   | Type | How to dispatch |
   |------|----------------|
   | `explore` | Task tool: `subagent_type: Explore`, prompt = task description |
   | `implement` | Task tool: `subagent_type: general-purpose`, prompt = task description |
   | `command` | Bash tool: run the command directly |
   | `skill:<name>` | Skill tool: `skill: <name>`, `args: <from description>` |

   **Parallel dispatch:** If multiple tasks are ready and `--sequential` is NOT set, dispatch them all in a **single message** with multiple tool calls. This is critical for performance.

   **Sequential dispatch:** If `--sequential` is set, dispatch one task at a time.

3. **Before dispatching each task**, call `TaskUpdate` to set `status: in_progress`.

4. **Sub-agent prompt template** — When dispatching `explore` or `implement` tasks via the Task tool, use this prompt structure:

   ```
   ## Task: <title>

   <description from Phase 1>

   ## Output Format

   When you are done, structure your final response as follows:

   RESULT: SUCCESS | PARTIAL | FAILED
   FILES_MODIFIED: <comma-separated list of files changed, or "none">
   SUMMARY: <1-3 sentence summary of what was done>
   ISSUES: <any problems encountered, or "none">
   ```

5. **Process results** — When a dispatched task completes:
   - Parse the result from the sub-agent output
   - Call `TaskUpdate` with `status: completed` on success
   - If the result is FAILED or PARTIAL:
     - **First failure:** Retry once with additional context from the error
     - **Second failure:** Mark the task as completed (to unblock dependents) and record the failure. Ask the user how to proceed using `AskUserQuestion`:
       - "Task '<title>' failed after retry. How to proceed?"
       - Options: "Skip and continue", "Retry with different approach", "Abort orchestration"
   - **Pass context forward:** If a completed task's output is needed by dependent tasks (e.g., file paths discovered, decisions made), include that context in the dependent task's prompt when dispatching it.

6. **Iterate** — After processing results, go back to step 1. Continue until all tasks are completed or the user aborts.

### Resumption (continue/resume)

1. Call `TaskList` to get current state.
2. If there are no tasks, inform the user: "No orchestration in progress. Start one with `/orchestrate <task>`."
3. If there are tasks, display the current state:
   - Completed tasks (with summaries if available)
   - In-progress tasks
   - Pending tasks (blocked and unblocked)
4. Resume from step 1 of Normal Execution (find ready tasks and dispatch).

---

## Phase 4: SYNTHESIZE — Report Results

After all tasks are completed (or aborted):

1. **Collect results** — Call `TaskList` to get final state of all tasks.

2. **Generate summary report:**

   ```
   ## Orchestration Complete

   **Task:** <original task description>

   ### Results
   - Completed: X/Y tasks
   - Failed: Z tasks (if any)

   ### What was done
   <Bullet list summarizing each completed task's outcome>

   ### Files modified
   <Deduplicated list of all files modified across sub-tasks>

   ### Issues encountered
   <Any problems or warnings from sub-tasks, or "None">

   ### Suggested next steps
   <Based on the task context, suggest logical follow-ups>
   ```

3. **Suggest skill chaining** — If appropriate, offer to run follow-up skills:
   - After code changes: "Run `/bramble-build` to verify the build?"
   - After successful build: "Run `/commit` to commit changes?"
   - After commit: "Run `/github-create-pr` to open a PR?"

   Use `AskUserQuestion` to offer these as options.

---

## Constraints

- **One level deep only** — Sub-agents dispatched via the Task tool cannot spawn their own sub-agents. All decomposition happens here in the orchestrator.
- **No shared state** — Sub-agents don't see each other's output. If task B depends on task A's output, you must include A's results in B's prompt when dispatching B.
- **Session-scoped** — TaskCreate/TaskList state does not persist across sessions. Resumption only works within the same session.
- **Skills from orchestrator only** — Sub-agents cannot invoke skills. If a sub-task requires a skill, type it as `skill:<name>` and the orchestrator calls the Skill tool directly.
- **Context budget** — Keep sub-agent prompts focused and concise. Include only the information the sub-agent needs, not the entire orchestration context.

## Example Invocations

```
/orchestrate Refactor the NeoPixel HAL to separate hardware access from color logic
/orchestrate --dry-run Add input validation to the LoRa message handler
/orchestrate --sequential 1. Read src/lora/sx1276.cpp 2. Summarize its structure 3. Add error logging
/orchestrate continue
```
