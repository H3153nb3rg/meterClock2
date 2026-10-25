"""Link the ESP-IDF OpenThread border-router glue library.

pioarduino's Arduino "hybrid compile" builds the OpenThread component with
``CONFIG_OPENTHREAD_BORDER_ROUTER=y``, but the hard-coded library list used for
the Arduino link does not include the prebuilt border-router glue archive
(``libopenthread_br.a``). Without it the link fails with undefined references to
``esp_openthread_border_router_init``, ``esp_openthread_set_backbone_netif``,
``otPlatInfraIf*``, the meshcop mDNS helpers, etc.

This script appends that archive to the link (only pulled members are linked, so
it costs nothing when the border router is not used).
"""

Import("env")

from pathlib import Path

mcu = env.BoardConfig().get("build.mcu", "esp32")
platform = env.PioPlatform()

archive = (
    Path(platform.get_package_dir("framework-espidf"))
    / "components"
    / "openthread"
    / "lib"
    / mcu
    / "libopenthread_br.a"
)

if archive.is_file():
    env.Append(LIBS=[env.File(str(archive))])
    print("Linking OpenThread border-router glue: %s" % archive.name)
