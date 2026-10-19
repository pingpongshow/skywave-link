# Skywave Link Test Suite

# Helper macro: add a test executable linked to skywave_link_core
macro(add_skywave_link_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(${name} PRIVATE skywave_link_core)
    target_compile_options(${name} PRIVATE ${SKYWAVE_LINK_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
endmacro()

add_skywave_link_test(test_lzh)
add_skywave_link_test(test_freq_manager)
add_skywave_link_test(test_config)
add_skywave_link_test(test_b2f)
add_skywave_link_test(test_message_store)
add_skywave_link_test(test_pop3)
add_skywave_link_test(test_smtp)
add_skywave_link_test(test_aprs)
add_skywave_link_test(test_modem_client)
add_skywave_link_test(test_file_transfer)
add_skywave_link_test(test_image_preview)
add_skywave_link_test(test_mesh_relay)
add_skywave_link_test(test_winlink_telnet)
add_skywave_link_test(test_radio_controller)
add_skywave_link_test(test_skywave_protocol)
add_skywave_link_test(test_app_helpers)
add_skywave_link_test(test_email_gateway_job)
