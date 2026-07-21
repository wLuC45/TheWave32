# Shared: patch libnet80211.a to neutralise ieee80211_raw_frame_sanity_check.
#
# IDF v5's libnet80211.a rejects any raw 802.11 frame whose source MAC
# (addr2) doesn't match this device's MAC. Modules that legitimately spoof
# the source MAC (deauth, beacon-spam, ghost) need this disabled.
#
# `-Wl,--wrap` doesn't work on Xtensa: linker relaxation resolves the call
# inside the same archive to the original symbol before the wrap takes
# effect. The patch script in firmware/scripts/patch_net80211.py rewrites
# the function body to `entry; movi.n a2, 0; retw.n` (always returns 0).
#
# Usage in a per-module top-level CMakeLists.txt, AFTER the project()
# declaration:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../cmake/net80211_patch.cmake)
#   tw32_apply_net80211_patch()

function(tw32_apply_net80211_patch)
    set(_patched_lib  "${CMAKE_BINARY_DIR}/libnet80211_patched.a")
    set(_patch_script "${CMAKE_CURRENT_LIST_DIR}/../scripts/patch_net80211.py")
    set(_orig_lib     "$ENV{IDF_PATH}/components/esp_wifi/lib/esp32s3/libnet80211.a")

    add_custom_command(
        OUTPUT  ${_patched_lib}
        COMMAND ${CMAKE_COMMAND} -E env python3
                ${_patch_script} ${_orig_lib} ${_patched_lib}
                ${CMAKE_AR}
        DEPENDS ${_patch_script} ${_orig_lib}
        COMMENT "Patching libnet80211.a (disabling raw-frame sanity check)"
        VERBATIM
    )
    add_custom_target(net80211_patch ALL DEPENDS ${_patched_lib})
    add_dependencies(${CMAKE_PROJECT_NAME}.elf net80211_patch)

    # Insert the patched archive on the linker command line BEFORE the
    # original. `--allow-multiple-definition` makes the first definition
    # win; the patched .o defines ieee80211_raw_frame_sanity_check first,
    # so the original is ignored at link time.
    target_link_libraries(${CMAKE_PROJECT_NAME}.elf PRIVATE
        "-Wl,--allow-multiple-definition"
        "${_patched_lib}"
    )
endfunction()
