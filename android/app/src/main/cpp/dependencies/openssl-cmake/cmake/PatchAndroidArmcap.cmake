# OpenSSL 3.3.2 ARM assembly uses R_ARM_REL32 references to this internal
# capability word. It must not be ELF-preemptible when libcrypto.a is linked
# into libsunshine.so with NDK r28 lld. Keep assembly acceleration enabled.
set(armcap_file "${OPENSSL_SOURCE_DIR}/crypto/armcap.c")
file(READ "${armcap_file}" armcap_source)
set(original "unsigned int OPENSSL_armcap_P = 0;")
set(hidden "__attribute__((visibility(\"hidden\"))) unsigned int OPENSSL_armcap_P = 0;")
string(FIND "${armcap_source}" "${hidden}" already_patched)
if(already_patched EQUAL -1)
    string(FIND "${armcap_source}" "${original}" declaration)
    if(declaration EQUAL -1)
        message(FATAL_ERROR "OpenSSL ARM capability declaration changed; review Android visibility patch")
    endif()
    string(REPLACE "${original}" "${hidden}" armcap_source "${armcap_source}")
    file(WRITE "${armcap_file}" "${armcap_source}")
endif()
