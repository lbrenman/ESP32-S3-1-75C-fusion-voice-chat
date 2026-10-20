# Third-party code in this project

| Path | Source | License |
| --- | --- | --- |
| `CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults` (first part), `components/bsp_extra/` | Waveshare [ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) example `examples/esp-idf/05_Spec_Analyzer` | Apache-2.0 (`components/bsp_extra/LICENSE`) |
| `main/minimp3.h` | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0 1.0 (`main/LICENSE.minimp3`) |

Changes to Waveshare's files: the project name in `CMakeLists.txt` is `voice_chat`
(was `lvgl_demo_v9`), and `sdkconfig.voice` is appended to `sdkconfig.defaults`.
