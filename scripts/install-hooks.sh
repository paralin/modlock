#!/bin/sh
# install-hooks installs the repository's pre-commit and commit-msg hooks into
# this checkout's hooks directory, beside any hooks other tools installed
# there. It installs only from the top of the package's own checkout: a copy
# inside another repository, such as an npm package unpacked under a logbook's
# .tmp/, would otherwise resolve that repository's hooks, which may be a
# tracked core.hooksPath directory, and overwrite them.
top=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
[ "$(cd "$top" && pwd -P)" = "$(pwd -P)" ] || exit 0
hooks=$(git rev-parse --git-path hooks)
mkdir -p "$hooks"
printf '#!/bin/sh\nexec bun scripts/pre-commit.ts\n' >"$hooks/pre-commit"
chmod +x "$hooks/pre-commit"
printf '#!/bin/sh\nexec sh scripts/commit-msg.sh "$1"\n' >"$hooks/commit-msg"
chmod +x "$hooks/commit-msg"
