# Cycle 11 implementation rules (all W agents)
- Work only in $HOME/src/Eglantine-c11 (branch mkw-c11). Plan: docs/cycle11-plan.md. Your files: section 3 table. Never edit a file/function another agent owns; if you need a hook there, write the exact lines in docs/cycle11-hooks-<you>.md.
- NEVER edit any CMakeLists.txt. New code is header-only (new headers under src/video_core/mt/), no new .cpp.
- Every piece behind its own EDEN_NX2_C11_* switch, plus the *_CHECK / *_STATS counters the plan names, logging a one-line startup state. Follow existing switch patterns (see mt/c10_front.h, mt/c10_vram.h).
- Do NOT build, do NOT run the emulator, do NOT commit, do NOT launch sub-agents. Another agent is measuring right now. The coordinator compiles and integrates once everyone is done. Be careful with syntax/includes since you can't compile; read surrounding code.
- Write docs/cycle11-notes-<you>.md: what you implemented, switches and defaults, risks, what to look at in the test round.
- Implement the whole workstream (all sub-items, contingent ones too behind off-by-default switches if data is missing). Big, complete change; no micro-steps.
