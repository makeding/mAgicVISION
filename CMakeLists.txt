cmake_minimum_required(VERSION 3.16)
project(mAgicVISION C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
find_package(Threads REQUIRED)

enable_testing()

add_executable(test_ts_receiver tests/test_ts_receiver.c userland/receiver.c)
target_link_libraries(test_ts_receiver PRIVATE mb86H_core Threads::Threads)
target_compile_options(test_ts_receiver PRIVATE -Wall -Wextra -Werror)

add_library(mb86H_core STATIC core/ts_framer.c core/receiver.c core/ts_mux.c core/protocol.c core/firmware_ping.c
            core/card.c core/card_atr.c core/frontend.c core/session.c core/device_query.c core/stream_input.c core/resident_image.c core/bootstrap.c core/loader.c core/loader_snapshot.c core/recovery.c core/native_download.c)
target_include_directories(mb86H_core PUBLIC include)
target_compile_options(mb86H_core PRIVATE -Wall -Wextra -Werror)

add_library(mb86H_device STATIC device/device.c device/firmware.c device/gv_mvp_xz/device.c device/gv_mvp_xz/boot.c
            device/torne/device.c device/pix_dt295/device.c device/pix_dt295/boot.c)
target_include_directories(mb86H_device PUBLIC include)
target_compile_options(mb86H_device PRIVATE -Wall -Wextra -Werror)
add_library(mb86H_userland STATIC userland/receiver.c)
target_link_libraries(mb86H_userland PUBLIC mb86H_core Threads::Threads)
target_compile_options(mb86H_userland PRIVATE -Wall -Wextra -Werror)
add_library(usb_protocol STATIC usb/window.c usb/boot.c)
target_include_directories(usb_protocol PUBLIC include)
target_compile_options(usb_protocol PRIVATE -Wall -Wextra -Werror)
target_link_libraries(usb_protocol PUBLIC mb86H_core)
target_link_libraries(mb86H_device PUBLIC usb_protocol)


add_executable(test_ts_framer tests/test_ts_framer.c)
target_link_libraries(test_ts_framer PRIVATE mb86H_core)
target_compile_options(test_ts_framer PRIVATE -Wall -Wextra -Werror)

add_executable(test_firmware_ping tests/test_firmware_ping.c)
target_link_libraries(test_firmware_ping PRIVATE mb86H_core)
target_compile_options(test_firmware_ping PRIVATE -Wall -Wextra -Werror)

add_executable(test_usb_window tests/test_usb_window.c)
target_link_libraries(test_usb_window PRIVATE usb_protocol)
target_compile_options(test_usb_window PRIVATE -Wall -Wextra -Werror)
add_test(NAME usb_window COMMAND test_usb_window)

add_executable(test_device_boot tests/test_device_boot.c)
target_link_libraries(test_device_boot PRIVATE mb86H_device)
target_compile_options(test_device_boot PRIVATE -Wall -Wextra -Werror)
add_test(NAME device_boot COMMAND test_device_boot)

add_executable(test_native_download tests/test_native_download.c)
target_link_libraries(test_native_download PRIVATE mb86H_device)
target_compile_options(test_native_download PRIVATE -Wall -Wextra -Werror)
add_test(NAME native_download COMMAND test_native_download)

add_executable(test_ts_mux tests/test_ts_mux.c)
target_link_libraries(test_ts_mux PRIVATE mb86H_core)
target_compile_options(test_ts_mux PRIVATE -Wall -Wextra -Werror)
add_test(NAME ts_mux COMMAND test_ts_mux)
add_test(NAME ts_receiver COMMAND test_ts_receiver)
add_test(NAME ts_framer COMMAND test_ts_framer)
add_test(NAME firmware_ping COMMAND test_firmware_ping)

add_executable(test_dvb_request tests/test_dvb_request.c)
target_link_libraries(test_dvb_request PRIVATE mb86H_core mb86H_device)
target_compile_options(test_dvb_request PRIVATE -Wall -Wextra -Werror)
add_test(NAME dvb_request COMMAND test_dvb_request)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()

if(TARGET PkgConfig::LIBUSB)
    add_library(usb_libusb_transport STATIC usb/userland/libusb_device.c
                usb/userland/libusb_control.c usb/userland/libusb_stream.c
                usb/userland/libusb_boot.c usb/userland/libusb.c)
    target_include_directories(usb_libusb_transport PUBLIC include)
    target_link_libraries(usb_libusb_transport PUBLIC PkgConfig::LIBUSB Threads::Threads
                          mb86H_core usb_protocol mb86H_device)
    target_compile_options(usb_libusb_transport PRIVATE -Wall -Wextra -Werror)

    add_executable(test_libusb_transport tests/test_libusb_transport.c)
    target_include_directories(test_libusb_transport PRIVATE include)
    target_link_libraries(test_libusb_transport PRIVATE PkgConfig::LIBUSB Threads::Threads
                          mb86H_core usb_protocol mb86H_device)
    target_compile_options(test_libusb_transport PRIVATE -Wall -Wextra -Werror)
    add_test(NAME libusb_transport COMMAND test_libusb_transport)

    add_executable(usbboot userland/cli/usbboot.c)
    target_link_libraries(usbboot PRIVATE usb_libusb_transport mb86H_userland)
    target_compile_options(usbboot PRIVATE -Wall -Wextra -Werror)
    add_executable(test_capture_pat tests/test_capture_pat.c)
    target_link_libraries(test_capture_pat PRIVATE usb_libusb_transport mb86H_userland)
    target_compile_options(test_capture_pat PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME capture_pat COMMAND test_capture_pat)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ccid_metadata COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ccid_metadata.py)
        add_test(NAME dvb_alias COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dvb_alias.py
                 --cc ${CMAKE_C_COMPILER}
                 --out ${CMAKE_CURRENT_BINARY_DIR}/test_dvb_alias)
        add_test(NAME ccid_descriptors COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ccid_descriptors.py
                 --cc ${CMAKE_C_COMPILER}
                 --out ${CMAKE_CURRENT_BINARY_DIR}/test_ccid_descriptors)
        add_test(NAME ccid_queue COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ccid_queue.py
                 --cc ${CMAKE_C_COMPILER}
                 --out ${CMAKE_CURRENT_BINARY_DIR}/test_ccid_queue)
    endif()
else()
    message(STATUS "libusb-1.0 development files absent: skipping libusb transport")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME usb_ccid_optional COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_usb_ccid_optional.py
             --cc ${CMAKE_C_COMPILER}
             --out ${CMAKE_CURRENT_BINARY_DIR}/test_usb_ccid_optional)
endif()

add_executable(test_loader tests/test_loader.c)
target_link_libraries(test_loader PRIVATE mb86H_core mb86H_device)
target_compile_options(test_loader PRIVATE -Wall -Wextra -Werror)
add_test(NAME loader COMMAND test_loader)

add_executable(test_loader_snapshot tests/test_loader_snapshot.c)
target_link_libraries(test_loader_snapshot PRIVATE mb86H_core)
target_compile_options(test_loader_snapshot PRIVATE -Wall -Wextra -Werror)
add_test(NAME loader_snapshot COMMAND test_loader_snapshot)

add_executable(test_session tests/test_session.c)
target_link_libraries(test_session PRIVATE mb86H_core)
target_compile_options(test_session PRIVATE -Wall -Wextra -Werror)
add_test(NAME session COMMAND test_session)

add_executable(test_stream_input tests/test_stream_input.c)
target_link_libraries(test_stream_input PRIVATE mb86H_core)
target_compile_options(test_stream_input PRIVATE -Wall -Wextra -Werror)
add_test(NAME stream_input COMMAND test_stream_input)

add_executable(test_card tests/test_card.c)
target_link_libraries(test_card PRIVATE mb86H_core)
target_compile_options(test_card PRIVATE -Wall -Wextra -Werror)
add_test(NAME card COMMAND test_card)

add_executable(test_pcsc_atr tests/test_pcsc_atr.c)
target_link_libraries(test_pcsc_atr PRIVATE mb86H_core)
target_compile_options(test_pcsc_atr PRIVATE -Wall -Wextra -Werror)
add_test(NAME pcsc_atr COMMAND test_pcsc_atr)

# These offline tests use assert for both checks and exercised calls.
# Keep their assertions enabled when the host libraries use Release flags.
get_property(mb86H_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS mb86H_targets)
    if(target MATCHES "^test_")
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endforeach()

add_executable(test_bootstrap tests/test_bootstrap.c)
target_link_libraries(test_bootstrap PRIVATE mb86H_core)
target_compile_options(test_bootstrap PRIVATE -Wall -Wextra -Werror -UNDEBUG)
set(MB86H_BOOTSTRAP_FIXTURE "" CACHE FILEPATH
    "Optional SBC2 artifact for the bootstrap oracle test")
if(MB86H_BOOTSTRAP_FIXTURE)
    add_test(NAME bootstrap COMMAND test_bootstrap "${MB86H_BOOTSTRAP_FIXTURE}")
else()
    add_test(NAME bootstrap COMMAND test_bootstrap)
endif()
