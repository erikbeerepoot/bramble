# Sort Tmux Windows

Reorganize tmux windows by category for a cleaner workspace.

## Instructions

1. **List all windows** to see current state:
   ```bash
   tmux list-windows -F '#{window_index}: #{window_id} - #{pane_current_command} (#{pane_title})'
   ```

2. **Categorize windows** into groups (in order of priority):
   - **Claude**: Windows running claude code (command contains version like `2.1.x`)
   - **SSH**: Windows with ssh sessions
   - **Git**: Windows running git commands
   - **Shell**: Windows running zsh, bash, or similar shells
   - **Other**: Everything else (node, vim, etc.)

3. **Reorder windows** using this approach:
   - Move all windows to temporary high positions (100+) first to avoid index conflicts
   - Then move them back to positions 1, 2, 3... in the desired category order

   ```bash
   # Example: Move window @0 to temp position, then back to final position
   tmux move-window -s @{window_id} -t {temp_position}
   tmux move-window -s {temp_position} -t {final_position}
   ```

4. **Verify the result**:
   ```bash
   tmux list-windows -F '#{window_index}: #{pane_current_command} (#{pane_title})'
   ```

## Example Output

After sorting, windows should be ordered like:
```
1: claude (Project A)
2: claude (Project B)
3: ssh (server1)
4: ssh (server2)
5: git (repo)
6: zsh (local)
7: node (dev server)
```

## Notes

- Window IDs (@0, @1, etc.) are stable across moves; use them for reliable targeting
- The current window will remain selected after reordering
- If a category has no windows, skip it and continue with the next
