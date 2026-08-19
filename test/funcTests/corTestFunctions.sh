#
# corTestFunctions.sh - repo-specific test functions for corRest
#
export COR_BROKER=/home/kz/git/corRest/corRestTest

# Override coraineStart: corRestTest uses --port (double dash via parseArgs, not kargs)
# Actually corRestTest uses -port (single dash), so the default coraineStart works.
