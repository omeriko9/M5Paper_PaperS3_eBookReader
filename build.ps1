# Set error action to stop on any error
$ErrorActionPreference = "Stop"

# Full clean
idf.py fullclean
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Set target to esp32
idf.py set-target esp32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Set ESP port
$env:ESPPORT = "COM3"

# Build, flash, and monitor
idf.py build flash monitor
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }