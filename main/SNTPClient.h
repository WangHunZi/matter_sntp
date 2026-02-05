#pragma once

#include <openthread/instance.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SNTPCallback)(uint64_t time, int error, void *context);

void SNTPClientInit(struct otInstance *aInstance);
void SNTPClientQuery(const char *hostname, SNTPCallback callback, void *context);

#ifdef __cplusplus
}
#endif