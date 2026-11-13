# Container builds

Build the toolchain with `docker build -t sd2psx-build misc/docker`.
Run `docker run --rm -v "${PWD}:/src" -v sd2psx-build:/build sd2psx-build bash /src/misc/docker/build.sh` for SD2PSX release/debug outputs in `build/docker/`.
Use `misc/docker/build_release.sh` instead to build all five supported release variants into `build/release/`. That helper replaces existing UF2s in its output directory, so copy artifacts elsewhere before running it.

These helpers do not require folder cards or USB storage. The release helper reports whether those optional features are present by inspecting symbols. For validation, use fresh output directories and preserve the actual CMake build exit status; the helpers filter console output with grep.

## Current MCA regression checks

The 1.5.0 tag retains an older `tests/mca/run_tests.sh` and fixtures for removed `mca_comms` and bridge APIs. `sh tests/mca_current/run.sh` provides targeted checks using the current protocol, bridge and real PS2 command handlers: framing, ownership, Mass handover, reset, timeout, page admission, raw/page reads and writes, ECC, commit, persistent modes and backend selection. The original fixtures are retained for further migration; this is not a claim that every legacy MCA case has been ported. Use `sh tests/msc/run.sh` for the MSC checks.
