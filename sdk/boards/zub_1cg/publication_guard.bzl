"""Publication-policy checks for board artifact packages."""

def reject_restricted_sources(paths):
    if paths:
        fail("generated psu_init.tcl is restricted local output; keep it under ignored generated/")
