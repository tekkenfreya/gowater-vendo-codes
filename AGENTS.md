                                                                                                                                                                                                                                        # Repository Guidelines

## Project Structure & Module Organization
- Root holds the Arduino sketch `water_vending_demo.ino` controlling vending hardware.
- `CLAUDE.md` documents agent coordination requirements; keep AGENTS.md aligned.
- Add future support modules under `src/` and hardware docs under `docs/` to keep the root tidy.

## Build, Test, and Development Commands
- `arduino-cli compile --fqbn arduino:avr:uno water_vending_demo.ino` compiles the sketch; adjust the FQBN to match the deployed board.
- `arduino-cli upload --port COM3 --fqbn arduino:avr:uno water_vending_demo.ino` flashes firmware; substitute the correct serial port.
- `arduino-cli monitor --port COM3 --config 9600` tails serial logs for runtime diagnostics.

## Coding Style & Naming Conventions
- Follow Arduino/C++ style: 2-space indentation, braces on the same line, and descriptive camelCase for functions and variables (e.g., `dispenseVolumeMl`).
- Group hardware pin constants at the top in `kPinName` form and comment wiring assumptions.
- Prefer small, single-purpose functions; avoid dynamic allocation on microcontrollers.
- Run `clang-format -style=Google water_vending_demo.ino` before committing when the tool is available.

## Testing Guidelines
- Use `arduino-cli compile --warnings all` to fail builds on warnings.
- Where feasible, create simulation helpers that gate hardware calls behind `#ifdef UNIT_TEST` for host-side validation.
- Document manual test cases (button presses, flow sensor readings) in commit or PR notes until an automated harness exists.

## Commit & Pull Request Guidelines
- Write imperative commit subjects <= 72 chars (e.g., `Adjust flow sensor debounce`); include scope tags like `[hw]` when helpful.
- Reference related issue IDs in the body and note hardware test status.
- PRs should summarize intent, list verification steps (compilation, hardware run), attach photos/logs when behavior changes, and call out follow-up work.

## Security & Configuration Tips
- Never hardcode API keys or Wi-Fi credentials; load them from `secrets.h` excluded via `.gitignore`.
- Scrub serial logs before sharing; they may include customer IDs or payment tokens.
