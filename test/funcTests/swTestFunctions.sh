#
# swTestFunctions.sh - repo-specific test functions for swRest
#
export SW_BROKER=/home/kz/git/swRest/swRestTest

# Override swBrokerStart: swRestTest uses --port (double dash via parseArgs, not kargs)
# Actually swRestTest uses -port (single dash), so the default swBrokerStart works.
