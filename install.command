#!/bin/bash
# Install FINAL FANTASY XIV (1.23b) on this Mac from the retail disc. Mount the disc (or its image),
# then double-click this file, or paste into Terminal:
#
#   curl -fsSL https://raw.githubusercontent.com/rubymatrix/client-recomp/main/install.command | bash -s -- --server <host>
#
# It gets Apple's command line tools when they are missing (a dialog: click Install), puts the
# sources in ~/Library/Application Support/FFXIV1_0/source, and runs tools/xiv_install.py there:
# the disc is unpacked, its programs are translated and compiled on this Mac, and
# "FINAL FANTASY XIV.app" lands in /Applications. Nothing from the disc leaves this Mac.
#
# Options go to tools/xiv_install.py: --server <host> (the patch certificate is fetched from it),
# --account-url <url>, --disc <folder>, --install-dir <folder>, --patch-cert <file> (instead of
# fetching it). Run it again to update the sources and resume.
# XIV_SOURCE (folder), XIV_REPO (git URL or path) and XIV_BRANCH (default main) override where the
# sources go, where they come from and which branch.

# Everything is inside main, so a download cut short runs nothing.
main() {
    set -e
    local repo="${XIV_REPO:-https://github.com/rubymatrix/client-recomp.git}"
    local branch="${XIV_BRANCH:-main}"
    local src="${XIV_SOURCE:-$HOME/Library/Application Support/FFXIV1_0/source}"

    if [ "$(uname -s)" != "Darwin" ] || [ "$(uname -m)" != "arm64" ]; then
        echo "FINAL FANTASY XIV for Mac needs a Mac with Apple silicon (M1 or later)." >&2
        exit 1
    fi

    if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang >/dev/null 2>&1; then
        echo "First, this Mac needs Apple's command line tools (free, about 1 GB)."
        echo "A dialog opens: click Install. This carries on when they are in."
        xcode-select --install >/dev/null 2>&1 || true
        until xcode-select -p >/dev/null 2>&1 && xcrun --find clang >/dev/null 2>&1; do
            sleep 5
        done
        echo "Command line tools installed."
    fi

    if [ -d "$src/.git" ]; then
        echo "Updating the sources in $src"
        git -C "$src" fetch --depth 1 origin "$branch"
        git -C "$src" checkout -q -B "$branch" FETCH_HEAD
    else
        echo "Getting the sources into $src"
        mkdir -p "$(dirname "$src")"
        git clone --depth 1 --branch "$branch" "$repo" "$src"
    fi

    # relative paths (--patch-cert, --install-dir, --disc) are the player's, from where they ran this
    export XIV_CALLER_DIR="${XIV_CALLER_DIR:-$PWD}"
    cd "$src"
    /usr/bin/python3 tools/xiv_install.py --open "$@"
}

main "$@"
