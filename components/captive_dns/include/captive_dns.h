/**
 * @file captive_dns.h
 * @brief Tiny DNS responder for captive-portal behaviour in AP mode.
 *
 * Answers every A query with the device address so that phones and
 * laptops open the configuration page automatically after joining the
 * access point. Only started while the access point is active.
 */
#pragma once

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t captive_dns_start(esp_ip4_addr_t answer_ip);
void      captive_dns_stop(void);

#ifdef __cplusplus
}
#endif
