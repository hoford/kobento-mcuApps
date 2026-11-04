# Third-party notices and credits

This repository's own code is under the Apache License 2.0 ([LICENSE](LICENSE)). The software below
belongs to its authors, keeps its own licence, and is gratefully credited here.

## In this repository

| Software | Where | Authors | Licence |
|---|---|---|---|
| Muse Gadget SDK `partitions.csv`, modified | `esp32/firmware/muse-gadget/partitions.csv` | Meta Platforms, Inc. and affiliates | Apache-2.0 (file header) |
| IoT panels and other sample documents | `documents/` | derived from [RemoteCompose-Examples](https://github.com/hoford/RemoteCompose-Examples) | Apache-2.0 here |

## Used by the build, not in this repository

These are fetched or expected beside this repository, and they end up in the binaries you build.

| Software | Authors | Licence |
|---|---|---|
| [kobento-mcu](../kobento-mcu), with everything in its `THIRD_PARTY_NOTICES.md`: ThorVG, FreeType-derived code, the fonts (one under SIL OFL 1.1), the RemoteCompose engine and more | see that file | see that file |
| [kobento-net](../kobento-net) | The Kobento Authors | Apache-2.0 |
| Muse Gadget SDK (`muse-gadget-sdk`) | Meta Platforms, Inc. and affiliates | Apache-2.0 |
| [ESP-IDF](https://github.com/espressif/esp-idf), with the libraries it bundles, such as FreeRTOS, lwIP and Mbed TLS | Espressif Systems and others | Apache-2.0; see ESP-IDF's copyright notes for the rest |
| [cJSON](https://github.com/DaveGamble/cJSON) (`espressif/cjson`) | Dave Gamble and cJSON contributors | MIT |
| Espressif components used by Wi-Fi on the Tab5: `esp_hosted`, `esp_wifi_remote` and their dependencies | Espressif Systems | Apache-2.0 |

Home Assistant is only talked to over its API; none of its code is included.
