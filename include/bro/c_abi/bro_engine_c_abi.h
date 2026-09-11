#ifndef BRO_ENGINE_C_ABI_H
#define BRO_ENGINE_C_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

void  bro_set_active_engine(void* engine);
void* bro_get_active_engine(void);

typedef struct BroTimeBridge {
    double (*getTimeScale)(void);
    void   (*setTimeScale)(double scale);
    bool   (*getTimePaused)(void);
    void   (*setTimePaused)(bool paused);
    double (*getTimeNowMs)(void);
} BroTimeBridge;

void bro_set_time_bridge(const BroTimeBridge* bridge);
const BroTimeBridge* bro_get_time_bridge(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_ENGINE_C_ABI_H
