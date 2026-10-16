file(GLOB_RECURSE src_files "*.c*")


set(EZLOPI_UTIL_COMPONENTS
        ezlopi-util-trace
        # Per-connection authorisation. Named explicitly rather than inherited
        # through ezlopi-core-offline-login, because this file calls
        # EZPI_util_wssession_open/close/reset directly from the accept and
        # close hooks.
        ezlopi-util-wssession
)

set(SDK_COMPONENTS
        esp_http_server
)

set(EZLOPI_CORE_COMPONENTS
        ezlopi-core-api
        ezlopi-core-wifi
        ezlopi-core-buffer
        ezlopi-core-ezlopi
        ezlopi-core-factory-info
        ezlopi-core-offline-login
)

set(EZLOPI_CLOUD_COMPONENTS

)



idf_component_register(SRCS "ezlopi_service_ws_server.c" "ezlopi_service_ws_server_clients.c" "${src_files}"
    INCLUDE_DIRS "."
    REQUIRES    ${SDK_COMPONENTS}
                ${EZLOPI_CORE_COMPONENTS}
                ${EZLOPI_UTIL_COMPONENTS}
                # ${EZLOPI_CLOUD_COMPONENTS}
    # The local configuration page, linked into the image as
    # _binary_config_ui_html_start/_end and answered to a plain HTTP GET on the
    # websocket port. See the block comment above ezpi_serve_config_page().
    #
    # TXTFILES rather than BINARIES so a NUL is appended; the length is still
    # computed from end-start rather than with strlen, see there.
    #
    # Embedded uncompressed: the page is 36554 bytes and costs 36512 bytes of
    # flash, measured as the esp32 image delta. With it in, the four CI targets
    # leave 339/639/324/267 KiB free in the 0x1EFC00 ota_0 partition
    # (ezlopi_partitions_v3_1_4_181_4mb.csv at the repo root). It gzips to
    # 12099 bytes, which was measured and not taken, but claiming that saving
    # means either committing a .html.gz next to the .html -- two files that
    # can drift apart with nothing to catch it -- or adding gzip as a
    # host-build dependency, which ESP-IDF does not itself require. Neither is
    # worth 24 KB today. If the headroom gets tight the second is the better of
    # the two, with Content-Encoding set in the handler.
    EMBED_TXTFILES "config_ui.html"
    )
