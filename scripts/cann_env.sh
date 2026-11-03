#!/usr/bin/env bash
# Source from build.sh / launch.sh; do not execute in a child process.
prl_load_cann() {
  local env_file="${CANN_ENV:-}" root="${CANN_ROOT:-}" candidate arch module
  if [[ -z "$env_file" ]]; then
    if [[ -n "$root" ]]; then
      env_file="$root/set_env.sh"
    else
      for candidate in /usr/local/Ascend/cann/set_env.sh /usr/local/Ascend/ascend-toolkit/latest/set_env.sh "$HOME/Ascend/cann/set_env.sh" "$HOME/Ascend/ascend-toolkit/latest/set_env.sh"; do
        if [[ -f "$candidate" ]]; then env_file="$candidate"; break; fi
      done
    fi
  fi
  [[ -f "$env_file" ]] || { echo "Set CANN_ENV to the installed set_env.sh" >&2; return 1; }
  # Vendor environment scripts may reference unset optional variables.
  local had_u=0
  [[ $- == *u* ]] && had_u=1 && set +u
  if source "$env_file"; then
    [[ $had_u == 1 ]] && set -u
  else
    [[ $had_u == 1 ]] && set -u
    return 1
  fi
  export CANN_ROOT="${root:-$(cd "$(dirname "$env_file")" && pwd -P)}"
}
prl_load_asc_modules() {
  if [[ -n "${ASC_MODULES:-}" && -f "$ASC_MODULES/FindASC.cmake" ]]; then return 0; fi
  local arch candidate module
  arch=$(uname -m)
  for candidate in "$CANN_ROOT/$arch-linux/tikcpp/ascendc_kernel_cmake/asc_modules" "$CANN_ROOT/compiler/tikcpp/ascendc_kernel_cmake/asc_modules" "$CANN_ROOT/tools/tikcpp/ascendc_kernel_cmake/asc_modules"; do
    if [[ -f "$candidate/FindASC.cmake" ]]; then export ASC_MODULES="$candidate"; return 0; fi
  done
  module=$(find -L "$CANN_ROOT" -type f -name FindASC.cmake -print -quit 2>/dev/null) || return 1
  [[ -n "$module" ]] || { echo "FindASC.cmake missing: install the CANN development toolchain or set ASC_MODULES" >&2; return 1; }
  export ASC_MODULES="${module%/*}"
}
