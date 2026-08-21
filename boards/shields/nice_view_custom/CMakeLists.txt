if(CONFIG_ZMK_DISPLAY)
    zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)

    if(CONFIG_NICE_VIEW_WIDGET_STATUS)
        target_sources(app PRIVATE
            widgets/util.c
            widgets/bolt.c
            widgets/bongocatart.c
            widgets/bongo_status.c
            custom_status_screen.c
        )
    endif()

    target_include_directories(app PRIVATE include)
endif()
