# Preferred Operating Methods & Orchestration Guidelines

## 1. Task Execution & Polling
- **Never poll tasks in a loop**: Do not poll `manage_task status` or sleep-loop while background tasks or commands run.
- **Wait for reactive wakeups**: The system automatically delivers notifications and wakes up the agent upon task completion or messages. Launch the task and wait for the system notification.

## 2. Subagent Orchestration for Large Tasks
- **Capacity estimation**: A well-instructed subagent can write ~4k lines of clean code or ~2k lines equivalent in debugging.
- **Orchestration**: Behave as an orchestrator and organize the work into sequential/parallel chunks of about the amount needed.
- **Incremental verification & commits**: Validate the work between subagents, committing as you go.
- **Concurrency limit**: Keep concurrent subagents to a maximum of 4 in parallel.
- **Stall checking**: When subagents run long tasks, check on them once every 20 minutes to ensure they have not stalled.

## 3. Code Decomposition & File Size Limits
- Keep all source files strictly under 1,000 lines.
- If a file grows beyond 1,000 lines, properly decompose it into smaller, focused modules.
- While rare exceptions may exist, files over 2,000 lines must be decomposed.

## 4. Multi-Repo & Submodule Workflow
- Standalone sibling repositories (`broaudio`, `broflora`, `brogameagent`, `brotensor`, `brolm`, `brosoundml`, `brodiffusion`, `brovisionml`, `bromesh`, `broimage`, etc.) own their native code and Bronze JavaScript APIs (`<sibling>_api`).
- **Submodule pinning**: Do NOT update or pin git submodules after every commit. Only update/pin submodules at the end of a full session before pushing.
