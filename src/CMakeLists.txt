idf_component_register(SRCS "main.cpp" "secure_identity_compile_proof.cpp" "secure_identity.cpp"
                           "ble_provisioning_transport.cpp" "esp_idf_wifi_connection_adapter.cpp"
                       INCLUDE_DIRS "../include"
                       REQUIRES esp-tls mbedtls efuse nvs_flash bt esp_wifi esp_event esp_netif)
