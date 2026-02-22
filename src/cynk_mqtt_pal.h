#ifndef CYNK_MQTT_PAL_H
#define CYNK_MQTT_PAL_H

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "compat.h"

#define MQTT_PAL_HTONS(s) compat_htons(s)
#define MQTT_PAL_NTOHS(s) compat_ntohs(s)

#define MQTT_PAL_TIME() time(NULL)

typedef time_t mqtt_pal_time_t;
typedef compat_mutex_t mqtt_pal_mutex_t;

#define MQTT_PAL_MUTEX_INIT(mtx_ptr) compat_mutex_init(mtx_ptr)
#define MQTT_PAL_MUTEX_LOCK(mtx_ptr) compat_mutex_lock(mtx_ptr)
#define MQTT_PAL_MUTEX_UNLOCK(mtx_ptr) compat_mutex_unlock(mtx_ptr)

struct cynk_mqtt_socket;
typedef struct cynk_mqtt_socket *mqtt_pal_socket_handle;

ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf, size_t len,
                         int flags);
ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buf, size_t bufsz,
                         int flags);

#endif
