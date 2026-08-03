idf_component_register(SRCS "main.cpp" "secure_identity_compile_proof.cpp" "secure_identity.cpp" INCLUDE_DIRS "../include" REQUIRES esp-tls mbedtls efuse nvs_flash)
