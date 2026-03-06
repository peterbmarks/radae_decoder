add_executable(test_eoo_callsign
    test_source.cpp
    ${CMAKE_SOURCE_DIR}/src/eoo/EooCallsignCodec.cpp
)

target_include_directories(test_eoo_callsign PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME eoo_callsign COMMAND test_eoo_callsign)
