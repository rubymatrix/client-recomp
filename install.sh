#!/bin/bash
# Install FINAL FANTASY XI on this Mac. Paste into Terminal:
#
#   curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-mac/main/install.sh | bash
#
# Run the same line again to update: it fetches the newest sources and rebuilds what changed.
# Options go to tools/setup.py after `bash -s --`, e.g.
#   curl -fsSL .../install.sh | bash -s -- --game ~/Games/"FINAL FANTASY XI"
#
# It gets Xcode's command line tools when they are missing (clang, git: a dialog from Apple, click
# Install), puts the sources in ~/Library/Application Support/FFXIRecompile/source, and runs
# tools/setup.py there: the game's code is translated and compiled on this Mac from the player's own
# game files, and "Final Fantasy XI.app" lands in /Applications.
#
# FFXI_SOURCE (folder), FFXI_REPO (git URL) and FFXI_BRANCH (default main) override where the sources
# go, where they come from and which branch.

# Everything is inside main, so a download cut short runs nothing.
main() {
    set -e
    local repo="${FFXI_REPO:-https://github.com/rubymatrix/xi-on-mac.git}"
    local branch="${FFXI_BRANCH:-main}"
    local src="${FFXI_SOURCE:-$HOME/Library/Application Support/FFXIRecompile/source}"

    if [ "$(uname -s)" != "Darwin" ]; then
        echo "This installer is for macOS. On Windows, see the README." >&2
        exit 1
    fi
    if [ "$(uname -m)" != "arm64" ]; then
        echo "FINAL FANTASY XI for Mac needs Apple silicon (M1 or later)." >&2
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

    # relative folders (--game, --dats, answers) are the player's, from where they ran this
    export FFXI_CALLER_DIR="$PWD"
    cd "$src"
    # stdin is this script when piped from curl; setup asks its questions on the terminal
    if (exec </dev/tty) 2>/dev/null; then
        /usr/bin/python3 tools/setup.py --open "$@" </dev/tty
    else
        /usr/bin/python3 tools/setup.py --open "$@"
    fi
}

main "$@"
