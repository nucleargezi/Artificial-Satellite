# `yrs test_template` Design

## Overview

This document defines a new `cpcli` command, `yrs test_template`, for validating an algorithm template repository against an online judge by reusing the existing submit workflow.

The initial target repository is the template library located at `/home/yorisou/Yorisou_alg_space/YRS`, but the design must support configuration so the command can later be reused in GitHub Actions and other environments.

The command's job is to:

- compare two git revisions in the template repository
- determine which template tests must be rerun
- submit the selected tests to the online judge
- persist a machine-readable status snapshot for later reporting, including static site generation

The first version intentionally favors correctness and simplicity over maximal performance:

- change detection is based on `git diff <base>...<head>`
- dependency discovery is rebuilt at runtime by scanning all test files
- test execution runs to completion across all selected tests before returning
- results are written as a full snapshot file instead of an incremental cache

## Goals

- Add a first-class `yrs test_template` command to `cpcli`
- Reuse the existing bundling and submit pipeline instead of introducing a second submission path
- Detect affected tests from template repository changes
- Rerun tests when a test source file changes or when one of its transitive template header dependencies changes
- Persist enough structured state to support future static pages that display template health
- Make local CLI usage and GitHub Actions share the same core behavior

## Non-Goals

- Parallel submission in the first version
- Maintaining a persistent dependency index between runs
- Supporting arbitrary languages beyond the existing C++ submit flow
- Reconstructing historical time series or long-term analytics in the first version
- Handling changes outside the configured template repository as test triggers

## Repository Model

`cpcli` and the template library are separate repositories.

- `cpcli` hosts the CLI implementation
- the template library repository is the git repository that `yrs test_template` analyzes
- the configured repository root is expected to be the directory that contains the template headers and the `test/` directory
- the existing `library_root` config remains the base for include resolution during bundling
- `template_repo_root` is the git root used for diffing, test discovery, and state output
- in the current setup, `template_repo_root` is a child of `library_root`

For the current setup, that repository root is `/home/yorisou/Yorisou_alg_space/YRS`.

## Configuration

Add a new config section in `yrs.toml`:

```toml
[template_test]
template_repo_root = "/home/yorisou/Yorisou_alg_space/YRS"
state_file = ".yrs/test_template/state.json"
default_language = "GNU G++17"
```

### Fields

- `template_repo_root`
  - Absolute path to the template library git repository
  - This path is the base for git operations, test discovery, and persisted state output
  - It is expected to be located under the existing `library_root` so visited include files can be mapped back to repository-relative paths
- `state_file`
  - Path to the generated status snapshot
  - Relative paths are resolved under `template_repo_root`
  - The parent directory should be created automatically if missing
- `default_language`
  - Default submit language label passed to the existing submit flow
  - Can be overridden by a future CLI flag if needed

## Test File Conventions

The command assumes test programs live under `test/**/*.cpp` within `template_repo_root`.

Each test file must follow these conventions:

- The first line is a comment that stores the problem URL
- The URL points to the target online judge problem page
- The problem id is derived from that URL
- The file includes template headers using the same include patterns supported by the existing bundler

Example:

```cpp
// https://icpc.bjtu.edu.cn/problem/10539
#include "YRS/all.hpp"
```

### Parsing Rules

- The first line is trimmed and must begin with `//`
- The remaining text is parsed as a URL
- The problem id is extracted from the URL path
- If the URL is malformed or the problem id cannot be extracted, the test is recorded as failed input metadata rather than aborting the entire command

## CLI Interface

The initial interface is:

```text
yrs test_template --base <rev> [--head <rev>] [--json] [--all] [--filter <pattern>] [--max-cases <n>]
```

### Arguments

- `--base <rev>`
  - Required
  - Base revision for `git diff`
- `--head <rev>`
  - Optional
  - Defaults to `HEAD`
- `--json`
  - Prints the run summary in JSON instead of human-readable text
- `--all`
  - Ignores diff selection and reruns all discovered tests
- `--filter <pattern>`
  - Restricts execution to matching test paths
  - Intended for local debugging and partial reruns
- `--max-cases <n>`
  - Truncates the selected test list after filtering
  - Intended for local debugging

## High-Level Flow

For a normal diff-based run, the command performs the following steps:

1. Load configuration and validate the template test section
2. Resolve and validate `template_repo_root`
3. Run `git diff <base>...<head>` inside the template repository
4. Discover all `test/**/*.cpp` files under the template repository
5. For each discovered test:
   - parse the first-line problem URL
   - derive problem metadata
   - compute transitive local template dependencies by reusing bundler expansion logic
   - normalize visited files under `template_repo_root` into repository-relative paths for diff matching and persisted output
6. Select tests that must be rerun
7. Execute the selected tests sequentially through the existing submit workflow
8. Rebuild the persisted state snapshot from the current repository scan and latest execution results
9. Print the run summary and exit with the appropriate status code

When `--all` is present, step 3 is skipped for selection purposes, though repository metadata may still record the provided revisions.

## Dependency Discovery

The command must not maintain a dependency cache in the first version.

Instead, each run rebuilds dependency information by scanning all discovered test files.

### Reuse of Existing Logic

The current bundler already:

- expands local `#include "..."` directives
- resolves includes relative to the current file and `library_root`
- walks transitive local includes

`yrs test_template` should reuse that traversal logic or extract it into a shared helper that returns both:

- bundled content for submission
- the set of visited local file dependencies

This avoids introducing a second C++ include parser and keeps selection logic aligned with real submission behavior.

### Dependency Set

For each test, record:

- the test file path
- the transitive set of local files visited during include expansion
- a filtered subset of those dependencies that are template headers, for example `**/*.hpp`
- repository-relative dependency paths for every visited file that lives under `template_repo_root`

The test source file itself is tracked separately from header dependencies so the trigger reason can distinguish:

- `test_file_changed`
- `dependency_changed`
- `manual`

## Test Selection Rules

Given the changed paths from `git diff <base>...<head>`, select a test for rerun when any of the following is true:

1. The test file itself is added, modified, renamed into place, or otherwise changed under `test/**/*.cpp`
2. At least one changed template header is present in the test's transitive dependency set
3. `--all` is present

### Files That Trigger No Test by Default

- Changes outside `template_repo_root`
- Non-header template files that are not test sources
- Documentation, scripts, or configuration files that are not part of the include graph
- Visited include files outside `template_repo_root`, because they cannot appear in the template repository diff

This keeps the default behavior aligned with the stated requirement: only new or changed test files and changed dependent template headers trigger reruns.

## Submission Strategy

The first version executes selected tests sequentially.

### Reasons

- Avoid stressing the online judge
- Avoid ambiguity when multiple submissions from the same account appear close together
- Simplify failure handling and output ordering

The command reuses the existing submit workflow:

- bundle the selected source
- submit to the online judge
- poll for the final verdict
- fetch optional detail fields such as judge detail and compile error

## Persisted State File

Persist a single snapshot file at the configured `state_file` path, for example:

`/home/yorisou/Yorisou_alg_space/YRS/.yrs/test_template/state.json`

This file represents the latest known state after a run and is intended to become the data source for future static pages.

### Top-Level Shape

```json
{
  "schema_version": 1,
  "generated_at": "2026-04-05T12:34:56Z",
  "repo": {
    "root": "/home/yorisou/Yorisou_alg_space/YRS",
    "base": "main",
    "head": "HEAD"
  },
  "latest_run": {
    "trigger_mode": "diff",
    "changed_paths": ["po/comp_inv.hpp"],
    "selected_tests": ["test/fps/comp_inv.cpp"],
    "total_discovered": 20,
    "total_selected": 1,
    "passed": 1,
    "failed": 0
  },
  "tests": {
    "test/fps/comp_inv.cpp": {
      "path": "test/fps/comp_inv.cpp",
      "problem_url": "https://icpc.bjtu.edu.cn/problem/10539",
      "problem_id": 10539,
      "dependencies": ["po/comp_inv.hpp", "aa/main.hpp"],
      "last_trigger": "dependency_changed",
      "last_tested_head": "abc1234",
      "last_updated_at": "2026-04-05T12:34:56Z",
      "last_result": {
        "status": "passed",
        "verdict": "Accepted",
        "grade": "100",
        "time_text": "31MS",
        "memory_text": "4096K",
        "run_id": 123456,
        "judge_detail": null,
        "compile_error": null,
        "runtime_error": null
      }
    }
  }
}
```

### State Semantics

- The file structure is fully regenerated on each successful command run from the current repository scan
- Deleted tests naturally disappear from the snapshot
- Tests that were discovered but not selected still appear in `tests`
- For unexecuted tests, the command should inherit the previous snapshot's last known result and timestamps when available
- For tests with no previous recorded result, `last_result` may be `null` until the first successful execution
- If future history is needed, it should be added as a separate artifact instead of overloading this snapshot

### Recommended Per-Test Fields

- `path`
- `problem_url`
- `problem_id`
- `dependencies`
- `last_trigger`
- `last_tested_head`
- `last_updated_at`
- `last_result`

### Recommended `last_result.status` Values

- `passed`
- `failed`
- `invalid`

`invalid` covers local metadata or parsing problems, such as a missing or malformed problem URL.

## Error Handling

The command should distinguish between:

- test-level failures
- command-level failures

### Test-Level Failures

These are recorded per test and do not stop the overall run:

- online judge verdicts such as `WA`, `TLE`, `MLE`, `RE`, or `CE`
- malformed first-line problem URL
- missing or unparseable problem id
- other per-test metadata problems that still allow the command to continue scanning or running remaining tests

These tests count toward the failed count in the run summary.

### Command-Level Failures

These abort the command because the system cannot produce a trustworthy run summary:

- missing or invalid `template_test` config
- `template_repo_root` does not exist or is not a git repository
- `git diff` invocation fails
- test discovery fails globally
- state file cannot be written
- shared submit infrastructure fails in a way that prevents subsequent tests from running reliably

## Exit Codes

- `0`
  - the command completed successfully and every selected test passed
- `1`
  - the command completed successfully but at least one selected test failed or was invalid
- `2`
  - the command failed at the system level and could not produce a trustworthy result set

## Output

### Human-Readable Output

The default terminal output should include:

- compared revision range
- number of changed paths
- total discovered tests
- selected tests
- pass/fail totals
- compact failure summaries containing test path, verdict or error type, and problem URL

### JSON Output

When `--json` is present, print a machine-readable summary for the current run.

The JSON summary does not replace the persisted snapshot. The snapshot is always the long-lived artifact; the JSON stdout is only for immediate integration, such as CI logs or workflow steps.

## GitHub Actions Fit

The same command should be usable in GitHub Actions without changing selection logic.

Typical CI usage:

```text
yrs test_template --base origin/main --head HEAD --json
```

The workflow can then:

- fail the job when the command exits with `1` or `2`
- upload or publish the generated `state.json`
- feed the snapshot into a later static-site generation step

## Implementation Notes

The cleanest implementation path is:

1. add template-test configuration types to the config layer
2. extract or extend bundler traversal so a caller can obtain visited include files
3. add a core `test_template` workflow module in `yrs-core`
4. add a CLI wrapper in `yrs-cli`
5. serialize the state snapshot with `serde`
6. add tests for config loading, diff selection, dependency selection, invalid URL handling, and state file generation

## Testing Strategy

The implementation should include automated tests for:

- config parsing of the new `[template_test]` section
- diff-based selection of changed test files
- diff-based selection through transitive header dependencies
- no-op selections when changes are unrelated
- URL parsing success and failure cases
- state snapshot serialization and overwrite behavior
- exit code mapping for full pass, partial fail, and system error cases

## Rationale

This design intentionally chooses full dependency reconstruction at runtime.

Why this is the right first version:

- it eliminates stale-index risk
- it keeps dependency selection aligned with real submission bundling
- it is simple enough to trust in CI
- it still produces durable output for future reporting layers

If runtime cost later becomes a problem, caching can be added as an optimization after the correctness model is already stable.
