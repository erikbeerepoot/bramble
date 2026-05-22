---
name: github-create-issue
description: Creates a GitHub issue from conversation context or user-provided description.
---

# Create GitHub Issue

## Instructions

1. Review the conversation context to identify:
   - The problem or feature being discussed
   - Any proposed solutions or requirements
   - Relevant technical details

2. If the user provided a specific description or quoted text, use that as the basis for the issue

3. Create a well-structured issue with:
   - **Title**: Clear, concise summary (imperative mood, e.g., "Add subsecond precision to timestamps")
   - **Body** containing:
     - `## Problem` - What's the issue or missing feature?
     - `## Proposed Solution` - How should it be addressed?
     - `## Benefits` (optional) - Why is this valuable?
     - Code examples if relevant

4. Create the issue using `gh issue create`:
   ```bash
   gh issue create --title "Title here" --body "$(cat <<'EOF'
   ## Problem
   ...

   ## Proposed Solution
   ...

   EOF
   )"
   ```

5. Report the issue URL to the user

## Arguments

- Optional: `[title]` - If provided, use this as the issue title
- Optional: `[description]` - If provided, use this as additional context

## Notes

- If context is ambiguous, ask the user for clarification before creating
- Keep titles under 70 characters
- Use markdown formatting in the body for readability
