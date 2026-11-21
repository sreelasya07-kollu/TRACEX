#!/usr/bin/env bash
# Claude Code PreToolUse hook: protect the locked project charter.
#   exit 2 = block the tool call (stderr goes back to Claude), exit 0 = allow.
#
# Only these fields are inspected (never `description` or edit contents):
#   Bash, PowerShell                      -> tool_input.command
#   Edit, Write, MultiEdit, NotebookEdit  -> tool_input.file_path / notebook_path
# Best effort only: the permission deny rules in .claude/settings.json are the
# primary barrier. If bash/awk/grep are missing this hook fails open.

INPUT=$(cat)

# Print the decoded JSON string value of `want`. tool_name is read at the top
# level; any other key is read directly inside the top-level tool_input object.
json_get() {
  printf '%s' "$INPUT" | awk -v want="$1" '
    { buf = buf $0 "\n" }
    END {
      hex = "0123456789abcdef"
      n = length(buf); depth = 0; iskey = 0
      for (i = 1; i <= n; i++) {
        c = substr(buf, i, 1)
        if (c == "{" || c == "[") {
          depth++; ctype[depth] = c; key[depth] = ""; iskey = (c == "{")
        } else if (c == "}" || c == "]") {
          depth--; iskey = 0
        } else if (c == ",") {
          iskey = (ctype[depth] == "{")
        } else if (c == ":") {
          iskey = 0
        } else if (c == "\"") {
          s = ""
          for (i++; i <= n; i++) {
            c = substr(buf, i, 1)
            if (c == "\"") break
            if (c != "\\") { s = s c; continue }
            e = substr(buf, i + 1, 1); i++
            if (e == "n") s = s "\n"
            else if (e == "t") s = s "\t"
            else if (e == "r") s = s "\r"
            else if (e == "b" || e == "f") s = s " "
            else if (e == "u") {
              h = tolower(substr(buf, i + 1, 4)); i += 4
              code = (index(hex, substr(h, 3, 1)) - 1) * 16 + index(hex, substr(h, 4, 1)) - 1
              if (substr(h, 1, 2) == "00" && code > 0) s = s sprintf("%c", code)
              else s = s "?"
            } else s = s e
          }
          if (ctype[depth] == "{" && iskey) {
            key[depth] = s
          } else if (want == "tool_name" && depth == 1 && key[1] == "tool_name") {
            printf "%s", s; exit
          } else if (want != "tool_name" && depth == 2 && ctype[2] == "{" \
                     && key[1] == "tool_input" && key[2] == want) {
            printf "%s", s; exit
          }
        }
      }
    }'
}

block() {
  echo "BLOCKED: $1 The project charter is locked; use docs/change-requests/ instead." >&2
  exit 2
}

# Protected file names (case-insensitive, any directory, \ or / separators).
NAMES='(00-project-charter\.md|\.charter\.sha256)'

TOOL=$(json_get tool_name)

case "$TOOL" in
  Edit|Write|MultiEdit|NotebookEdit)
    for field in file_path notebook_path; do
      P=$(json_get "$field")
      if [ -n "$P" ] && printf '%s\n' "$P" | grep -Eiq "(^|[\\/])${NAMES}\$"; then
        block "$TOOL on a protected charter file."
      fi
    done
    exit 0
    ;;
  Bash|PowerShell)
    CMD=$(json_get command)
    [ -n "$CMD" ] || exit 0
    has() { printf '%s\n' "$CMD" | grep -Eiq -- "$1"; }

    # Word start / word end around a command name.
    B='(^|[^[:alnum:]_.-])'
    E='([^[:alnum:]_-]|$)'
    # Optional directory prefix: "quoted path with spaces\", 'quoted/', or bare/path/.
    DIRP="(\"[^\"]*[\\/]|'[^']*[\\/]|[^[:space:];&|\"']*[\\/])?"
    LOCK="${DIRP}lock-charter\.sh"

    # 1) Running lock-charter.sh (reading it is fine).
    #    a) via an interpreter anywhere in the command (bare, as a path, quoted,
    #       .exe; any prefix such as then/do/xargs/timeout N/uv run before it).
    INTERP="(${DIRP}(ba|z|da|k)?sh(\.exe)?[\"']?([[:space:]]+-[[:alnum:]-]+)*|source|\.)"
    if has "${B}${INTERP}[[:space:]]+${LOCK}"; then
      block "only the owner may run scripts/lock-charter.sh."
    fi
    #    b) executed directly at command position.
    PREFIX="(then|do|else|xargs|exec|nohup|sudo|env|command|time|timeout[[:space:]]+[^[:space:]]+|uv[[:space:]]+run)"
    if has "(^|[;&|(\`{]|\\\$\\(|${B}${PREFIX}[[:space:]])[[:space:]]*${LOCK}"; then
      block "only the owner may run scripts/lock-charter.sh."
    fi

    # Everything below needs a protected file named in the command.
    has "$NAMES" || exit 0

    # 2) A write verb used as a word (incl. PowerShell cmdlets and aliases).
    VERBS='(rm|mv|cp|tee|truncate|chmod|set-content|add-content|out-file|move-item|copy-item|remove-item|rename-item|clear-content'
    VERBS="${VERBS}|del|erase|move|copy|ren|ri|mi|cpi|rni|sc|ac|clc)"
    if has "${B}${VERBS}${E}"; then
      block "command modifies (or may modify) a protected charter file."
    fi
    if has "${B}sed[[:space:]]([^;&|]*[[:space:]])?(-[[:alpha:]]*i|--in-place)"; then
      block "sed -i on a protected charter file."
    fi
    if has "${B}perl[[:space:]]([^;&|]*[[:space:]])?-[[:alpha:]]*i"; then
      block "perl -i on a protected charter file."
    fi
    if has "${B}git[[:space:]]([^;&|]*[[:space:]])?(checkout|restore|reset|rm|mv)${E}"; then
      block "git command that rewrites a protected charter file."
    fi

    # 3) A > / >> / >| / &> redirect whose target is a protected file.
    #    `2>&1` is not a file target; `->` and `=>` are not redirects; the target
    #    must end at the protected name (`> charter.md.bak` is allowed).
    if has "(^|[^=>-])[0-9]*>>?\|?[[:space:]]*[\"']?[^[:space:];&|\"'<>]*${NAMES}([\"'[:space:];&|)]|\$)"; then
      block "redirect writes to a protected charter file."
    fi
    exit 0
    ;;
esac
exit 0
