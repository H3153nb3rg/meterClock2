"""Generate the assembler (.S) embeds for the managed components' server certs.

pioarduino's Arduino "hybrid compile" (custom_sdkconfig) rebuilds the ESP-IDF and
Arduino libraries with a nested CMake/Ninja build. The IDF components
``espressif__esp_insights`` and ``espressif__esp_rainmaker`` declare their server
certificates via ``target_add_binary_data()``, which expects ``<name>.crt.S`` files
in the build directory. PlatformIO's own embedding step for the ``arduino``
framework emits ``<name>.crt.txt.o`` objects instead, so the ``.S`` sources never
exist and the build fails with:

    Source `.pio/build/<env>/https_server.crt.S' not found

This script pre-generates those ``.S`` files using the same ESP-IDF helper
(``data_file_embed_asm.cmake``) that ``target_add_binary_data()`` uses, so the
embedded-certificate targets resolve.
"""

Import("env")

import subprocess
from pathlib import Path

CERT_RELATIVE_PATHS = [
    "managed_components/espressif__esp_insights/server_certs/mqtt_server.crt",
    "managed_components/espressif__esp_insights/server_certs/https_server.crt",
    "managed_components/espressif__esp_rainmaker/server_certs/rmaker_mqtt_server.crt",
    "managed_components/espressif__esp_rainmaker/server_certs/rmaker_claim_service_server.crt",
    "managed_components/espressif__esp_rainmaker/server_certs/rmaker_ota_server.crt",
]


def _generate():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))
    platform = env.PioPlatform()

    cmake = Path(platform.get_package_dir("tool-cmake")) / "bin" / "cmake"
    embed_script = (
        Path(platform.get_package_dir("framework-espidf"))
        / "tools" / "cmake" / "scripts" / "data_file_embed_asm.cmake"
    )

    if not embed_script.is_file():
        return

    build_dir.mkdir(parents=True, exist_ok=True)
    for relative_path in CERT_RELATIVE_PATHS:
        cert = project_dir / relative_path
        if not cert.is_file():
            continue
        asm = build_dir / (cert.name + ".S")
        subprocess.check_call(
            [
                str(cmake),
                "-DDATA_FILE=%s" % cert,
                "-DSOURCE_FILE=%s" % asm,
                "-DFILE_TYPE=TEXT",
                "-P",
                str(embed_script),
            ]
        )
        print("Prepared embedded certificate: %s" % asm.name)


_generate()
