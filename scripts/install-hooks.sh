#!/bin/sh
# install-hooks installs the repository's pre-commit and commit-msg hooks into
# this checkout's hooks directory, beside any hooks other tools installed
# there. Outside a git checkout it does nothing.
hooks=$(git rev-parse --git-path hooks 2>/dev/null) || exit 0
mkdir -p "$hooks"
printf '#!/bin/sh\nexec bun scripts/pre-commit.ts\n' >"$hooks/pre-commit"
chmod +x "$hooks/pre-commit"
printf '#!/bin/sh\nexec sh scripts/commit-msg.sh "$1"\n' >"$hooks/commit-msg"
chmod +x "$hooks/commit-msg"
