"""
PlatformIO pre-build extra script for StackChan firmware.

- Disables the IDF Component Manager to avoid re-downloading / version-
  resolution failures (PlatformIO bundles IDF 5.5.0; project targets 5.5.4).
- Manually adds the pre-fetched managed_components/ to CMake's
  EXTRA_COMPONENT_DIRS so every component is compiled and its include
  paths are registered correctly.
"""
Import("env")  # noqa: F821  — PlatformIO injects this

import os

# ── 1. Disable the IDF Component Manager ─────────────────────────────────────
env.Append(ENV={"IDF_COMPONENT_MANAGER": "0"})
print("[extra_script] IDF_COMPONENT_MANAGER=0 — using cached managed_components/")

# ── 2. Expose managed_components/ to CMake as EXTRA_COMPONENT_DIRS ───────────
project_dir = env.subst("$PROJECT_DIR")
managed_dir = os.path.join(project_dir, "managed_components")

if os.path.isdir(managed_dir):
    env.Append(
        CMAKE_EXTRA_ARGS=[f"-DEXTRA_COMPONENT_DIRS={managed_dir}"]
    )
    count = sum(1 for d in os.listdir(managed_dir)
                if os.path.isdir(os.path.join(managed_dir, d)))
    print(f"[extra_script] Added managed_components/ ({count} components) to CMake")
else:
    print("[extra_script] WARNING: managed_components/ not found — run fetch_repos.py first")
