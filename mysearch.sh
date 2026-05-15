#!/usr/bin/env bash
# mysearch.sh - mysearch-compatible wrapper using find | xargs | grep
#
# Usage:
#   ./mysearch ./src 'py|ts|tsx' 'reward_(type|field)'
#
# Arguments:
#   1. search root directory
#   2. extension regex, matched against the part after the final dot, without the dot
#   3. line regex, passed to grep -E
#
# Optional:
#   MYSEARCH_JOBS=N  Number of parallel xargs worker processes. Default: 1.
#
# Output:
#   path:line_number:line_content
#
# Exit code:
#   0 if at least one match, 1 if no matches, 2 on errors.

set -o pipefail

usage() {
  cat >&2 <<USAGE
usage: $0 <dir> <extension-regex> <line-regex>
example: $0 ./src 'py|ts|tsx' 'reward_(type|field)'

Environment:
  MYSEARCH_JOBS=N  Number of parallel xargs workers. Default: 1.
USAGE
}

if [[ $# -ne 3 ]]; then
  usage
  exit 2
fi

root=$1
ext_re=$2
line_re=$3

if [[ ! -d $root ]]; then
  printf 'mysearch: not a directory: %s\n' "$root" >&2
  exit 2
fi

jobs=${MYSEARCH_JOBS:-1}
if ! [[ $jobs =~ ^[1-9][0-9]*$ ]]; then
  printf 'mysearch: MYSEARCH_JOBS must be a positive integer: %s\n' "$jobs" >&2
  exit 2
fi

# Validate grep -E regex before running find/xargs.
grep_err=$(mktemp "${TMPDIR:-/tmp}/mysearch.grep_err.XXXXXX") || exit 2
grep -E -q -- "$line_re" /dev/null 2>"$grep_err"
grep_rc=$?
if [[ $grep_rc -eq 2 ]]; then
  printf 'mysearch: invalid line regex for grep -E: %s\n' "$line_re" >&2
  cat "$grep_err" >&2
  rm -f "$grep_err"
  exit 2
fi
rm -f "$grep_err"

match_flag=$(mktemp "${TMPDIR:-/tmp}/mysearch.match.XXXXXX") || exit 2
: > "$match_flag"
trap 'rm -f "$match_flag"' EXIT HUP INT TERM

run_grep_batch_simple() {
  xargs -0 -P "$jobs" bash -c '
    line_re=$1
    match_flag=$2
    shift 2

    if [[ $# -eq 0 ]]; then
      exit 0
    fi

    grep -HnE -- "$line_re" "$@"
    rc=$?

    case $rc in
      0)
        printf "1\n" > "$match_flag"
        exit 0
        ;;
      1)
        # grep found no matches in this batch. Not an error.
        exit 0
        ;;
      *)
        exit "$rc"
        ;;
    esac
  ' _ "$line_re" "$match_flag"
}

run_grep_batch_with_ext_filter() {
  xargs -0 -P "$jobs" bash -c '
    ext_re=$1
    line_re=$2
    match_flag=$3
    shift 3

    if [[ $# -eq 0 ]]; then
      exit 0
    fi

    full_ext_re="^(${ext_re})$"
    files=()

    for path in "$@"; do
      base=${path##*/}
      [[ $base == *.* ]] || continue
      ext=${base##*.}
      if [[ $ext =~ $full_ext_re ]]; then
        files+=("$path")
      fi
    done

    if [[ ${#files[@]} -eq 0 ]]; then
      exit 0
    fi

    grep -HnE -- "$line_re" "${files[@]}"
    rc=$?

    case $rc in
      0)
        printf "1\n" > "$match_flag"
        exit 0
        ;;
      1)
        # grep found no matches in this batch. Not an error.
        exit 0
        ;;
      *)
        exit "$rc"
        ;;
    esac
  ' _ "$ext_re" "$line_re" "$match_flag"
}

# Fast path: convert simple extension alternations like 'py|ts|tsx'
# into find predicates: -name '*.py' -o -name '*.ts' -o -name '*.tsx'.
# For more complex extension regexes, fall back to filtering inside xargs.
if [[ $ext_re =~ ^[A-Za-z0-9_]+(\|[A-Za-z0-9_]+)*$ ]]; then
  IFS='|' read -r -a exts <<< "$ext_re"

  find_args=("$root" -type f '(')
  for i in "${!exts[@]}"; do
    if [[ $i -gt 0 ]]; then
      find_args+=(-o)
    fi
    find_args+=(-name "*.${exts[$i]}")
  done
  find_args+=(')' -print0)

  find "${find_args[@]}" | run_grep_batch_simple
  pipeline_rc=$?
else
  # Validate extension regex for Bash's [[ =~ ]] engine.
  # Bash returns status 2 for a syntactically invalid regex.
  full_ext_re="^(${ext_re})$"
  [[ "" =~ $full_ext_re ]]
  case $? in
    2)
      printf 'mysearch: invalid extension regex for bash: %s\n' "$ext_re" >&2
      exit 2
      ;;
  esac

  find "$root" -type f -print0 | run_grep_batch_with_ext_filter
  pipeline_rc=$?
fi

if [[ $pipeline_rc -ne 0 ]]; then
  exit 2
fi

if [[ -s $match_flag ]]; then
  exit 0
fi

exit 1
