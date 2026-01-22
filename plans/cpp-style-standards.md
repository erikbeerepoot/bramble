# C++ Code Style Standards Plan

## Overview

This plan establishes C++ code style standards and tooling to ensure consistent, maintainable code throughout the bramble project. The codebase currently has no formal style configuration, though existing code follows reasonably consistent patterns.

## Current State

**Existing patterns observed:**
- 4-space indentation
- `#pragma once` for header guards
- CamelCase for classes, camelCase for methods
- UPPER_CASE for constants
- Doxygen-style documentation (@brief, @param, @return)
- C++17 with exceptions and RTTI enabled
- Opening brace on same line for classes, new line for function implementations

**What's missing:**
- No `.clang-format` configuration
- No `.clang-tidy` configuration
- No `.editorconfig` file
- No automated enforcement in CI
- No documented style guide

---

## Implementation Phases

### Phase 1: Document Style Standards

**Goal:** Create a formal style guide based on existing patterns.

**Tasks:**
1. [ ] Create `STYLE.md` documenting the project's C++ style conventions
2. [ ] Include naming conventions, formatting rules, and best practices
3. [ ] Add examples for common patterns

**Style Guide Contents:**
- Naming conventions (classes, functions, variables, constants)
- File organization (headers, includes, guards)
- Formatting (indentation, braces, line length)
- Documentation requirements (Doxygen tags)
- Modern C++ usage guidelines
- Embedded-specific considerations (memory, performance)

---

### Phase 2: Add Tooling Configuration

**Goal:** Configure automatic formatting and static analysis tools.

#### 2.1 EditorConfig (.editorconfig)

```ini
# EditorConfig for bramble project
root = true

[*]
indent_style = space
indent_size = 4
end_of_line = lf
charset = utf-8
trim_trailing_whitespace = true
insert_final_newline = true

[*.{c,cpp,h,hpp}]
indent_size = 4

[*.md]
trim_trailing_whitespace = false

[CMakeLists.txt]
indent_size = 4
```

#### 2.2 Clang-Format (.clang-format)

```yaml
---
Language: Cpp
BasedOnStyle: Google
IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 100
AccessModifierOffset: -4

# Braces
BreakBeforeBraces: Custom
BraceWrapping:
  AfterClass: false
  AfterFunction: true
  AfterNamespace: false
  AfterStruct: false
  AfterEnum: false
  BeforeElse: false
  BeforeWhile: false

# Alignment
AlignAfterOpenBracket: Align
AlignConsecutiveAssignments: false
AlignConsecutiveDeclarations: false
AlignOperands: true
AlignTrailingComments: true

# Includes
IncludeBlocks: Regroup
IncludeCategories:
  - Regex: '^<.*\.h>'
    Priority: 1
  - Regex: '^<.*>'
    Priority: 2
  - Regex: '^"pico/'
    Priority: 3
  - Regex: '^"hardware/'
    Priority: 4
  - Regex: '^".*"'
    Priority: 5

# Other
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
PointerAlignment: Right
SpaceAfterCStyleCast: false
SpaceBeforeParens: ControlStatements
Standard: c++17
```

#### 2.3 Clang-Tidy (.clang-tidy)

```yaml
---
Checks: >
  -*,
  bugprone-*,
  -bugprone-easily-swappable-parameters,
  clang-analyzer-*,
  cppcoreguidelines-init-variables,
  cppcoreguidelines-pro-type-member-init,
  misc-unused-parameters,
  modernize-use-nullptr,
  modernize-use-override,
  modernize-use-using,
  modernize-deprecated-headers,
  performance-*,
  readability-braces-around-statements,
  readability-const-return-type,
  readability-duplicate-include,
  readability-inconsistent-declaration-parameter-name,
  readability-redundant-smartptr-get,
  readability-simplify-boolean-expr

WarningsAsErrors: ''

HeaderFilterRegex: '.*'

CheckOptions:
  - key: readability-identifier-naming.ClassCase
    value: CamelCase
  - key: readability-identifier-naming.FunctionCase
    value: camelCase
  - key: readability-identifier-naming.VariableCase
    value: lower_case
  - key: readability-identifier-naming.ConstantCase
    value: UPPER_CASE
  - key: readability-identifier-naming.PrivateMemberSuffix
    value: '_'
```

**Tasks:**
1. [ ] Create `.editorconfig` file
2. [ ] Create `.clang-format` file matching existing code style
3. [ ] Create `.clang-tidy` configuration with embedded-appropriate checks
4. [ ] Test configurations against existing codebase
5. [ ] Document tool usage in README or CONTRIBUTING.md

---

### Phase 3: Apply Formatting to Existing Code

**Goal:** Format existing code to match standards (one-time cleanup).

**Approach:** Gradual, module-by-module to keep PRs reviewable.

**Tasks:**
1. [ ] Run clang-format on `src/hal/` directory
2. [ ] Run clang-format on `src/lora/` directory
3. [ ] Run clang-format on `src/storage/` directory
4. [ ] Run clang-format on `src/modes/` directory
5. [ ] Run clang-format on `src/config/` directory
6. [ ] Run clang-format on remaining files (main.cpp, tests, utils)
7. [ ] Review and commit each module separately

**Commands:**
```bash
# Format a single file
clang-format -i src/path/to/file.cpp

# Format all files in a directory
find src/hal -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Check formatting without modifying (useful for CI)
clang-format --dry-run --Werror src/**/*.cpp
```

---

### Phase 4: Add CI Enforcement (Optional)

**Goal:** Automatically check style compliance on PRs.

**Tasks:**
1. [ ] Add GitHub Actions workflow for format checking
2. [ ] Add clang-tidy checks to CI
3. [ ] Document how to fix formatting issues locally

**Example workflow (.github/workflows/style-check.yml):**
```yaml
name: Style Check

on: [pull_request]

jobs:
  format-check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install clang-format
        run: sudo apt-get install -y clang-format-14

      - name: Check formatting
        run: |
          find src -name "*.cpp" -o -name "*.h" | \
            xargs clang-format-14 --dry-run --Werror
```

---

### Phase 5: Add Pre-commit Hooks (Optional)

**Goal:** Catch style issues before commit.

**Tasks:**
1. [ ] Add `.pre-commit-config.yaml`
2. [ ] Document pre-commit setup in CONTRIBUTING.md

**Example (.pre-commit-config.yaml):**
```yaml
repos:
  - repo: https://github.com/pre-commit/mirrors-clang-format
    rev: v14.0.6
    hooks:
      - id: clang-format
        types_or: [c++, c]
```

---

## Style Guide Summary

### Naming Conventions

| Element | Style | Example |
|---------|-------|---------|
| Classes/Structs | CamelCase | `MessageHandler`, `NeoPixel` |
| Public methods | camelCase | `sendMessage()`, `setPixel()` |
| Private members | snake_case_ | `flash_size_`, `logger_` |
| Local variables | snake_case | `buffer_size`, `retry_count` |
| Constants | UPPER_CASE | `MAX_RETRIES`, `FLASH_PAGE_SIZE` |
| Macros | PREFIX_UPPER | `SX1276_REG_FIFO` |
| Enum types | CamelCase | `enum class WorkType` |
| Enum values | CamelCase | `WorkType::BacklogTransmit` |
| Files | snake_case | `message_handler.cpp` |

### Formatting Rules

- **Indentation:** 4 spaces (no tabs)
- **Line length:** 100 characters maximum
- **Braces:**
  - Classes/structs: opening brace on same line
  - Functions: opening brace on new line
  - Control statements: always use braces
- **Includes:** Group by type (system, SDK, local)

### Documentation Requirements

- All public classes need @brief description
- All public methods need @brief, @param, @return
- Complex algorithms need inline comments
- Use Doxygen format for API documentation

### Modern C++ Guidelines

- Prefer `enum class` over plain `enum`
- Use `constexpr` for compile-time constants
- Prefer smart pointers over raw pointers
- Use `auto` where type is obvious from context
- Use range-based for loops where applicable
- Apply `const` liberally (const correctness)
- Use `override` for virtual method overrides

### Embedded-Specific Guidelines

- Avoid dynamic memory allocation in hot paths
- Prefer stack allocation for small objects
- Use `static_assert` for compile-time validation
- Be explicit about struct packing for wire protocols
- Document memory/stack usage for critical functions

---

## Implementation Priority

| Phase | Priority | Effort | Value |
|-------|----------|--------|-------|
| Phase 1: Document standards | High | Low | High |
| Phase 2: Add tool configs | High | Medium | High |
| Phase 3: Format existing code | Medium | Medium | Medium |
| Phase 4: CI enforcement | Low | Low | Medium |
| Phase 5: Pre-commit hooks | Low | Low | Low |

**Recommended approach:** Complete Phases 1-2 first, then Phase 3 as a single formatting PR, then add CI in Phase 4.

---

## Risks and Considerations

1. **Large formatting PR:** Applying clang-format to entire codebase creates a large diff. Mitigate by doing module-by-module or all at once with clear commit message.

2. **Tool availability:** clang-format/clang-tidy may not be installed on all dev machines. Document installation requirements.

3. **Pico SDK compatibility:** Some Pico SDK patterns may conflict with strict style rules. May need to exclude SDK headers from checks.

4. **Breaking git blame:** One-time format commit will affect git blame history. Use `git blame --ignore-rev` or a `.git-blame-ignore-revs` file.

---

## Success Criteria

- [ ] Style guide documented and accessible
- [ ] Tooling configurations added to repository
- [ ] Existing code formatted consistently
- [ ] New code automatically checked (CI or pre-commit)
- [ ] Team understands and follows style standards
