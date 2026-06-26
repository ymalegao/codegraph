# Working in this repo

You are working in a code repository with a flat-file handoff for session
continuity. There is no graph or verification tooling. Use ordinary tools.

## Session lifecycle

- At session start, if `HANDOFF.md` exists, read it before substantial work.
- Treat the handoff as prior-session state, not as a replacement for the user's
  current request. The current request wins when they conflict.
- Before ending a turn after meaningful changes, write/overwrite `HANDOFF.md`
  if the work would not be obvious to a fresh agent.

## Discovery and verification

- Use `rg`, the compiler, and the tests for discovery.
- Once a symbol or path matters to an action, re-check it against the current
  code before acting:
  - confirm the file still exists at the path you expect (`ls`, `rg`);
  - confirm the symbol still exists and is where you think (`rg "name"`);
  - if a referenced file or symbol is missing, that is information, not a dead
    end — search for where it moved before editing.
- A tool error is not an empty result. A failed lookup means "unverified," not
  "absent." Report or resolve failures; never treat a failed search as proof a
  thing is gone.

## Before editing

- Re-verify that any file or symbol named in the handoff still exists and is
  current before you act on it.
- If the handoff names something that has moved, been renamed, or been deleted,
  find the current location/replacement first, then act.

## Handoff contract

Use `HANDOFF.md` for resumable session state. A useful handoff states:

- current objective and user-visible success condition;
- completed work and verification already run;
- exact next action, blockers, and unresolved questions;
- affected files and symbols, named explicitly (path + symbol name), so the
  next session can locate them.

Keep it concise and operational. Do not record transient progress chatter or
speculative ideas.