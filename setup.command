#!/bin/bash
# Set up FINAL FANTASY XI on this Mac: double-click this file in Finder, or run ./setup.command.
# Options go to tools/setup.py (--game <FINAL FANTASY XI folder>, --no-install, --open; --help).
#
# Everything the build needs comes with this folder except clang, which is part of Xcode's
# command line tools. When they are missing, this asks macOS to install them (a dialog: click
# Install) and waits.
cd "$(dirname "$0")" || exit 1

if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang >/dev/null 2>&1; then
    echo "FINAL FANTASY XI needs Xcode's command line tools (free, from Apple)."
    echo "A dialog opens: click Install, and setup carries on when they are in."
    xcode-select --install >/dev/null 2>&1
    until xcode-select -p >/dev/null 2>&1 && xcrun --find clang >/dev/null 2>&1; do
        sleep 5
    done
    echo "Command line tools installed."
fi

/usr/bin/python3 tools/setup.py --open "$@"
status=$?
if [ -t 0 ] && [ -z "$FFXI_SETUP_NO_WAIT" ]; then
    echo
    read -r -p "Press Return to close this window." _
fi
exit $status
