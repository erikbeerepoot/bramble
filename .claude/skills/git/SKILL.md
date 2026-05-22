---
name: git
description: Unified git operations via sub-agent. Keeps verbose output out of main context.
---

# Unified Git Skill

Route all git operations through a sub-agent to keep verbose output (diffs, logs, status) out of the main conversation. Only a concise summary flows back.

## Arguments

`$ARGUMENTS` — The git operation and any extra arguments. Format: `<operation> [args...]`

## Supported Operations

| Operation | Usage | Description |
|-----------|-------|-------------|
| `status` | `/git status` | Summarize working tree state |
| `commit` | `/git commit` or `/git commit "message"` | Analyze changes, create atomic commits |
| `branch` | `/git branch feature/foo` | Create and switch to a new branch |
| `checkout` | `/git checkout main` | Switch branches |
| `pr` | `/git pr` or `/git pr "title"` | Push and create a pull request |
| `merge` | `/git merge` or `/git merge 42` | Merge a PR after checking status |
| `push` | `/git push` | Push current branch to remote |
| `sync` | `/git sync` | Fetch and rebase on main |
| `stash` | `/git stash` or `/git stash pop` | Stash or restore changes |
| `log` | `/git log` | Show and summarize recent commits |
| `diff` | `/git diff` or `/git diff main` | Summarize changes |
| `address-comments` | `/git address-comments` | Fetch PR comments, fix issues, commit, push |
| `tag` | `/git tag v1.0.0` | Create and push a tag |

## Instructions

### Step 1: Parse the operation

Extract the operation name and remaining arguments from `$ARGUMENTS`:
- The first word is the operation (e.g., `commit`, `pr`, `status`)
- Everything after the first word is passed as extra args to the sub-agent
- If `$ARGUMENTS` is empty, default to `status`

### Step 2: Dispatch to sub-agent

Use the **Task** tool with `subagent_type: general-purpose` to dispatch the operation. Use the appropriate prompt from the operation reference below.

**Every sub-agent prompt MUST include these rules at the top:**

```
## Git Safety Rules
- NEVER commit directly to main/master. If on main, stop and report an error.
- NEVER force push (--force, --force-with-lease) unless the user explicitly said to.
- NEVER use --no-verify or skip hooks.
- NEVER amend commits unless explicitly asked.
- NEVER run destructive commands (reset --hard, clean -f, branch -D) unless explicitly asked.
- Use conventional commit style: <type>(<scope>): <description>
- Keep the first line under 72 characters, use imperative mood.
- Co-author line: Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>

## Output Format
When done, structure your FINAL response EXACTLY as:

RESULT: SUCCESS | FAILED
SUMMARY: <concise 1-3 line summary of what happened>
DETAILS: <optional extra info like PR URLs, commit SHAs, branch names>
```

### Step 3: Return the result

After the sub-agent completes, relay its structured response to the user. Do NOT echo raw git output — only the SUMMARY and DETAILS lines.

---

## Operation Reference

Use these prompts when dispatching each operation. Replace `{args}` with the extra arguments parsed in Step 1.

---

### `status`

```
{git safety rules}

Run `git status` and `git stash list`. Provide a concise summary:
- Current branch
- Ahead/behind remote
- Number of staged, unstaged, and untracked files (with filenames if < 10)
- Any active stashes

{output format}
```

---

### `commit`

```
{git safety rules}

Analyze all uncommitted changes and create atomic commits.

User-provided message (if any): {args}

Steps:
1. Run: git status, git diff, git diff --cached, git log --oneline -5
2. If on main/master, STOP and report FAILED — must be on a feature branch.
3. If no changes exist, report FAILED with "nothing to commit".
4. Group changes into logical atomic commits. Consider:
   - Files that change together for a single purpose
   - Separate features from refactors from fixes
   - Test files go with their implementation
   - Each commit should leave the codebase in a working state
5. For each commit group:
   a. Stage only the relevant files: git add <file1> <file2> ...
   b. Commit with conventional commit message using a HEREDOC:
      git commit -m "$(cat <<'EOF'
      <type>(<scope>): <description>

      Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
      EOF
      )"
   c. If user provided a message, use it for the primary commit.
6. Prefer more smaller commits over fewer large ones.

{output format}
Include commit SHAs and messages in DETAILS.
```

---

### `branch`

```
{git safety rules}

Create a new branch and switch to it.

Branch name: {args}

Steps:
1. If no branch name provided, report FAILED — a name is required.
2. Run: git checkout -b {args}
3. Confirm the branch was created.

{output format}
```

---

### `checkout`

```
{git safety rules}

Switch to the specified branch.

Target: {args}

Steps:
1. If no target provided, report FAILED — a branch name is required.
2. Check for uncommitted changes with git status. If present, warn in DETAILS but proceed.
3. Run: git checkout {args}
4. Confirm the switch.

{output format}
```

---

### `pr`

```
{git safety rules}

Push the current branch and create a pull request.

User-provided title (if any): {args}

Steps:
1. Run: git branch --show-current, git status, git log main..HEAD --oneline, git diff main...HEAD --stat
2. If on main/master, STOP and report FAILED.
3. If there are uncommitted changes, warn in DETAILS but proceed.
4. Push the branch: git push -u origin $(git branch --show-current)
5. Analyze the commits and diff to generate:
   - A clear, descriptive PR title (under 70 chars). Use user title if provided.
   - A summary body with bullet points of what changed
   - A test plan section
6. Create the PR using a HEREDOC for the body:
   gh pr create --title "<title>" --body "$(cat <<'EOF'
   ## Summary
   <bullet points>

   ## Test plan
   <checklist>

   Generated with Claude Code
   EOF
   )"
7. Report the PR URL in DETAILS.

{output format}
```

---

### `merge`

```
{git safety rules}

Merge a pull request.

PR number or args: {args}

Steps:
1. If no PR number provided, find it: gh pr list --head $(git branch --show-current) --json number --jq '.[0].number'
2. Check PR status: gh pr view <number> --json state,reviewDecision,statusCheckRollup
3. If checks are failing, report in DETAILS but proceed with merge.
4. Merge with squash: gh pr merge <number> --squash --delete-branch
5. Switch to main and pull: git checkout main && git pull

{output format}
Include the merged PR number and title in DETAILS.
```

---

### `push`

```
{git safety rules}

Push the current branch to remote.

Extra args: {args}

Steps:
1. Run: git branch --show-current
2. If on main/master, STOP and report FAILED — don't push directly to main.
3. Push: git push -u origin $(git branch --show-current) {args}
4. Report status.

{output format}
```

---

### `sync`

```
{git safety rules}

Sync the current branch with main.

Steps:
1. Run: git branch --show-current, git status
2. If there are uncommitted changes, stash them first: git stash
3. Fetch: git fetch origin
4. If on main: git pull origin main
5. If on a feature branch: git rebase origin/main
6. If changes were stashed: git stash pop
7. If rebase conflicts occur, report FAILED with conflict details.

{output format}
```

---

### `stash`

```
{git safety rules}

Manage git stash.

Sub-command: {args}

Steps:
1. If no args or args is empty: git stash push -m "stash from /git"
2. If args is "pop": git stash pop
3. If args is "list": git stash list
4. If args is "drop": git stash drop
5. If args starts with "pop", "apply", "drop", or "show" followed by a number: git stash {args}
6. Otherwise: git stash {args}

{output format}
```

---

### `log`

```
{git safety rules}

Show recent commit history.

Extra args: {args}

Steps:
1. Run: git log --oneline --graph --decorate -20 {args}
2. Summarize: current branch, total commits shown, key themes/patterns in recent work.

{output format}
Include the formatted log summary in DETAILS.
```

---

### `diff`

```
{git safety rules}

Summarize changes.

Diff target: {args}

Steps:
1. If no args: run git diff and git diff --cached to show all uncommitted changes.
2. If args provided (e.g., "main"): run git diff {args}
3. Summarize the diff:
   - Number of files changed
   - For each file: brief description of what changed (added, modified, deleted, and a short summary)
   - Total lines added/removed
4. Do NOT output the raw diff. Summarize it.

{output format}
Include the file-by-file summary in DETAILS.
```

---

### `address-comments`

```
{git safety rules}

Fetch PR review comments, address open issues, commit fixes, and push.

Extra args: {args}

Steps:
1. Get current branch: git branch --show-current
2. Find PR number: gh pr list --head <branch> --json number --jq '.[0].number'
3. If no PR found, report FAILED.
4. Fetch review comments:
   - gh api repos/:owner/:repo/pulls/<number>/comments --jq '.[] | {path: .path, line: .line, body: .body, id: .id}'
   - gh pr view <number> --json reviews --jq '.reviews[] | {body: .body, state: .state}'
5. Categorize comments as FIXED (already addressed in code) or OPEN (still needs work).
6. For each OPEN comment:
   a. Read the relevant file
   b. Apply the fix
   c. Verify the change makes sense
7. Stage and commit fixes:
   git add <changed files>
   git commit -m "$(cat <<'EOF'
   fix: address PR review comments

   Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
   EOF
   )"
8. Push: git push

{output format}
Include count of fixed vs already-resolved comments in DETAILS.
```

---

### `tag`

```
{git safety rules}

Create and push a git tag.

Tag specification: {args}

Steps:
1. If no args, report FAILED — a tag name is required.
2. Create the tag: git tag {args}
3. Push the tag: git push origin {args}

{output format}
Include the tag name and commit it points to in DETAILS.
```

---

## Error Handling

If `$ARGUMENTS` contains an unrecognized operation, report the error and list available operations. Do NOT dispatch a sub-agent.

## Examples

```
/git status              → Dispatches status sub-agent, returns branch + file summary
/git commit              → Analyzes changes, creates atomic commits, returns SHAs
/git commit "fix typo"   → Creates single commit with provided message
/git branch feature/foo  → Creates branch, returns confirmation
/git pr                  → Pushes, creates PR, returns URL
/git pr "Add logging"    → Pushes, creates PR with given title, returns URL
/git merge               → Merges current branch's PR, returns confirmation
/git merge 42            → Merges PR #42, returns confirmation
/git sync                → Fetches and rebases on main
/git diff main           → Summarizes diff against main
/git address-comments    → Fetches and fixes PR review comments
/git tag v1.0.0          → Creates and pushes tag
```
