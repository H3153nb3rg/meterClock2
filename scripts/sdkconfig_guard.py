"""Drop the cached ESP-IDF sdkconfig when ``custom_sdkconfig`` changes.

ESP-IDF only applies ``sdkconfig.defaults`` to symbols that are *not already
present* in ``sdkconfig``. pioarduino's Arduino "hybrid compile" rewrites
``sdkconfig.defaults`` on every build but leaves the generated
``sdkconfig.<env>`` (and therefore the compiled ``sdkconfig.h``) in place, so a
changed ``custom_sdkconfig`` value is silently ignored and the build keeps using
the previous configuration.

This script hashes the current ``custom_sdkconfig`` value and, when it differs
from the last build, removes ``sdkconfig.<env>`` so the configuration is
regenerated.
"""

Import("env")

import hashlib
from pathlib import Path

project_dir = Path(env.subst("$PROJECT_DIR"))
pioenv = env.subst("$PIOENV")

raw = env.GetProjectOption("custom_sdkconfig", "") or ""
digest = hashlib.md5(raw.encode("utf-8")).hexdigest()

marker_dir = project_dir / ".pio"
marker = marker_dir / ("sdkconfig.%s.hash" % pioenv)
current = project_dir / ("sdkconfig.%s" % pioenv)

previous = marker.read_text().strip() if marker.is_file() else None

if previous != digest:
    if current.is_file():
        current.unlink()
        print("custom_sdkconfig changed: removed stale %s" % current.name)
    marker_dir.mkdir(parents=True, exist_ok=True)
    marker.write_text(digest)
