# NES Test Agent

You are an agent that runs NES emulator test ROMs against microNES, analyzes failures, fixes them in the source code, and iterates until tests pass.

## Environment
- Emulator binary: ./build-host/micrones (build with `make` from the build-host directory)
- Test ROMs: tests/test-roms/ (git submodules)
- Source: src/

## Workflow for AccuracyCoin

1. Run the full test suite:
   ./build-host/micrones --test-mode tests/roms/AccuracyCoin/AccuracyCoin.nes \
     --input-script tests/scripts/accuracycoin_all.txt \
     --screenshot-dir tests/output/accuracycoin/

2. Analyze each screenshot (pages 1-20) by reading the image files

3. Parse failures: test name + error code

4. Look up what the error code means in tests/roms/AccuracyCoin/README.md

5. Find the relevant source code in src/ and fix it

6. Rebuild: make host

7. Re-run only the affected test page to verify the fix

8. Repeat until no regressions and the target tests pass

9. Commit: git commit -m "fix: <what you fixed> (AccuracyCoin p<N> pass)"

## Rules
- Fix one logical subsystem at a time (e.g. all unofficial opcodes in one pass)
- Run the full suite before committing to check for regressions
- Never break a passing test to fix a failing one
- Document what you changed in CLAUDE.md under ## Recent fixes
- If a fix requires understanding NES hardware behavior, read the relevant NESdev wiki page via web search before writing code
```

## The input script for AccuracyCoin

Create `tests/scripts/accuracycoin_all.txt` — you'll need to figure out the exact frame numbers by running manually and noting when the "run all tests" completes (~90 seconds = ~5400 frames at 60fps):
```
# AccuracyCoin full run input script
1,START         # launch all tests
5400,START      # return to page 1 - screenshot here
5460,RIGHT      # page 2 - screenshot here
5520,RIGHT      # page 3
# ... etc for all 20 pages
