# The integration player: the kobento-mcu demo documents + this repo's integration documents
# (the Wi-Fi screen, Home Assistant pages), with Wi-Fi, a kobento-net receiver and the Home
# Assistant client.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(RESCAN CONFIGURE_DEPENDS)
endif()
set(MCU_DOCS ${CMAKE_CURRENT_LIST_DIR}/../../../../../kobento-mcu/documents)  # demo docs (library)
set(APP_DOCS ${CMAKE_CURRENT_LIST_DIR}/../../../../documents)                  # integration docs
set(KNET_INC ${CMAKE_CURRENT_LIST_DIR}/../../../../../kobento-net/cpp/include) # kobento-net (header-only)

file(GLOB DOCS ${RESCAN} ${MCU_DOCS}/*.rc ${APP_DOCS}/*.rc)
file(GLOB DOCS_FIT ${RESCAN} ${MCU_DOCS}/fit/*.rc)
list(APPEND DOCS ${DOCS_FIT})
# Screens that need this player behind them (the Wi-Fi list), last in the long-press cycle.
file(GLOB DOCS_SYSTEM ${RESCAN} ${APP_DOCS}/system/*.rc)
list(APPEND DOCS ${DOCS_SYSTEM})
# 3D documents only where the renderer draws 3D (CONFIG_KOBENTOVG_3D; off on the Core2).
if(CONFIG_KOBENTOVG_3D)
    file(GLOB DOCS3D ${RESCAN} ${MCU_DOCS}/3d/*.rc)
    list(APPEND DOCS ${DOCS3D})
endif()

# A board that names its documents (CONFIG_RCPLAYER_DOCUMENTS) gets those, in that order.
# documents/wide/ (wide strip screens) and documents/ha/ (Home Assistant pages) are only
# embedded this way.
if(CONFIG_RCPLAYER_DOCUMENTS)
    file(GLOB DOCS_WIDE ${RESCAN} ${APP_DOCS}/wide/*.rc ${APP_DOCS}/ha/*.rc)
    list(APPEND DOCS ${DOCS_WIDE})
    separate_arguments(_wanted UNIX_COMMAND "${CONFIG_RCPLAYER_DOCUMENTS}")
    set(_picked "")
    foreach(w ${_wanted})
        set(_found "")
        foreach(doc ${DOCS})
            get_filename_component(n ${doc} NAME_WE)
            if(n STREQUAL w)
                set(_found ${doc})
            endif()
        endforeach()
        if(NOT _found)
            message(FATAL_ERROR "CONFIG_RCPLAYER_DOCUMENTS: no document '${w}'")
        endif()
        list(APPEND _picked ${_found})
    endforeach()
    set(DOCS ${_picked})
endif()

# Generate a table of the embedded documents so main.cpp can iterate them.
set(DOC_TABLE ${CMAKE_CURRENT_BINARY_DIR}/embedded_docs.h)
set(_decl "")
set(_rows "")
foreach(doc ${DOCS})
    get_filename_component(n ${doc} NAME_WE)
    string(MAKE_C_IDENTIFIER ${n}_rc sym)
    string(APPEND _decl "extern const uint8_t _binary_${sym}_start[];\nextern const uint8_t _binary_${sym}_end[];\n")
    set(fit false)
    if(doc IN_LIST DOCS_FIT)
        set(fit true)
    endif()
    string(APPEND _rows "    {\"${n}\", _binary_${sym}_start, _binary_${sym}_end, ${fit}},\n")
endforeach()
file(WRITE ${DOC_TABLE} "#pragma once\n#include <cstdint>\n${_decl}struct EmbeddedDoc { const char* name; const uint8_t* start; const uint8_t* end; bool fit; };\nstatic const EmbeddedDoc kEmbeddedDocs[] = {\n${_rows}};\n")

# A chip without its own radio reaches a co-processor through esp_hosted (the Tab5's C6).
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32p4")
    set(RADIO esp_hosted)
endif()
idf_component_register(SRCS main.cpp console.cpp net.cpp ble.cpp ha.cpp
                       INCLUDE_DIRS . ${CMAKE_CURRENT_BINARY_DIR}
                       PRIV_INCLUDE_DIRS ${KNET_INC}
                       REQUIRES rcstage board esp_timer mbedtls esp_wifi esp_netif esp_http_server esp_http_client nvs_flash bt ${RADIO}
                       EMBED_FILES ${DOCS})
