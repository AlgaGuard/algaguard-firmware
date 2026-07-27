#include "algaguard/secure_identity.hpp"

#include "esp_tls.h"

// Link-time proof for the ESP-IDF 6.0.1 DS path.  It deliberately accepts only
// an opaque DS context; a conventional client-key PEM buffer is absent.
extern "C" bool algaguard_esp_tls_ds_context_is_opaque(void* ds_context) {
#if defined(CONFIG_ESP_TLS_USE_DS_PERIPHERAL)
  esp_tls_cfg_t config{};
  config.ds_data = ds_context;
  return config.ds_data == ds_context && config.clientkey_buf == nullptr && config.clientkey_bytes == 0;
#else
  (void)ds_context;
  return false;
#endif
}
