include(ExternalProject)

if(NOT DEFINED RADAE_NOPY_URL)
    set(RADAE_NOPY_URL https://github.com/peterbmarks/radae_nopy/archive/712f2f645a5de46a1d2f66418680d84dc2c5c653.zip)
endif()

ExternalProject_Add(radae_nopy_src
    DOWNLOAD_EXTRACT_TIMESTAMP YES
    DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/.cache/radae_nopy
    SOURCE_DIR   ${CMAKE_SOURCE_DIR}/.cache/radae_nopy/src
    STAMP_DIR    ${CMAKE_SOURCE_DIR}/.cache/radae_nopy/stamp
    URL          ${RADAE_NOPY_URL}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND   ""
)

set(RADE_NOPY_SRC ${CMAKE_SOURCE_DIR}/.cache/radae_nopy/src/src CACHE INTERNAL "radae_nopy source directory")
