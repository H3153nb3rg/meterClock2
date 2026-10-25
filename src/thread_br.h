#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts the OpenThread border router. The Wi-Fi station interface is used as the
// backbone (infrastructure) link, so call this only after Wi-Fi is connected.
void thread_br_start(void);

#ifdef __cplusplus
}
#endif
