# Smart Commit - Atomic Commit Analyzer

Analyze the current git changes and propose a set of atomic commits using conventional commit style.

## Instructions

1. **Gather all changes** by running these commands:
   - `git status` - see all modified, staged, and untracked files
   - `git diff` - see unstaged changes
   - `git diff --cached` - see staged changes
   - `git diff HEAD` - see all changes (staged + unstaged) combined

2. **Analyze the changes** and group them into logical atomic units. Consider:
   - Files that change together for a single purpose
   - Separation of concerns (don't mix features with refactors)
   - Test files should typically go with their implementation
   - Config changes may be separate or bundled depending on context

3. **Propose atomic commits** using conventional commit format:
   ```
   <type>(<scope>): <description>

   [optional body]
   ```

   **Types:**
   - `feat` - new feature
   - `fix` - bug fix
   - `docs` - documentation only
   - `style` - formatting, no code change
   - `refactor` - code change that neither fixes nor adds
   - `test` - adding or updating tests
   - `chore` - build, config, tooling changes
   - `perf` - performance improvement

   **Scope** (optional): component or area affected (e.g., `api`, `auth`, `storage`)

4. **Output format** - Present the proposed commits as:

   ```
   ## Proposed Commits (in order)

   ### Commit 1: <type>(<scope>): <message>
   **Files:**
   - path/to/file1.py
   - path/to/file2.py

   **Rationale:** Why these changes belong together

   ---

   ### Commit 2: ...
   ```

5. **After presenting**, ask the user if they want to:
   - Proceed with creating these commits (you'll stage and commit each group)
   - Modify the grouping
   - See more details about specific changes

## Important Rules

- Keep commits atomic: each should be a single logical change
- Each commit should leave the codebase in a working state if possible
- Don't mix unrelated changes even if they're in the same file
- Prefer more smaller commits over fewer large ones
- Use imperative mood in commit messages ("add" not "added")
- Keep the first line under 72 characters
