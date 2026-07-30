set(ALGAGUARD_QRCODE_SOURCE
    "${CMAKE_SOURCE_DIR}/.pio/libdeps/esp32-s3-dev-qr-onboarding-demo/QRCode/src/qrcode.c")

idf_component_register(SRCS "main.cpp" "secure_identity_compile_proof.cpp" "secure_identity.cpp"
                           "ble_provisioning_transport.cpp" "esp_idf_wifi_connection_adapter.cpp"
                           "esp_qr_onboarding.cpp" "esp_qr_credential_bootstrap.cpp"
                       INCLUDE_DIRS "../include"
                       REQUIRES esp-tls mbedtls efuse nvs_flash bt esp_wifi esp_event esp_netif esp_http_client)

if(EXISTS "${ALGAGUARD_QRCODE_SOURCE}")
    add_library(algaguard_qrcode STATIC "${ALGAGUARD_QRCODE_SOURCE}")
    target_compile_options(algaguard_qrcode PRIVATE
        -std=gnu17 -Wno-error=unknown-pragmas -Wno-error=type-limits)
    target_include_directories(algaguard_qrcode PUBLIC
        "${CMAKE_SOURCE_DIR}/.pio/libdeps/esp32-s3-dev-qr-onboarding-demo/QRCode/src")
    target_link_libraries(${COMPONENT_LIB} PRIVATE algaguard_qrcode)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_SOURCE_DIR}/.pio/libdeps/esp32-s3-dev-qr-onboarding-demo/QRCode/src")
endif()
