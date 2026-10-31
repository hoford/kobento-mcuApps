# The gadget_ext.h hooks for the Muse Gadget SDK on a board with a screen, plus the
# embedded RC documents (documents/).
set(DOC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../../documents)
# Re-glob on every build so a new document is picked up (not allowed in IDF's script pass).
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(RESCAN CONFIGURE_DEPENDS)
endif()
file(GLOB DOCS ${RESCAN} ${DOC_DIR}/*.rc)
# fit/: documents designed at a fixed size, shown whole: laid out at their own size and
# scaled to the screen (rcstage::load fit).
file(GLOB DOCS_FIT ${RESCAN} ${DOC_DIR}/fit/*.rc)
list(APPEND DOCS ${DOCS_FIT})
# system/: the documents that need this player's actions (the Wi-Fi screen).
file(GLOB DOCS_SYSTEM ${RESCAN} ${DOC_DIR}/system/*.rc)
list(APPEND DOCS ${DOCS_SYSTEM})
# 3D documents only where the renderer draws 3D (CONFIG_KOBENTOVG_3D; off on the Core2).
if(CONFIG_KOBENTOVG_3D)
    file(GLOB DOCS3D ${RESCAN} ${DOC_DIR}/3d/*.rc)
    list(APPEND DOCS ${DOCS3D})
endif()

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

# WHOLE_ARCHIVE: the hooks override weak defaults in main, so nothing would pull them in.
idf_component_register(SRCS muse_rc.cpp commands.cpp sleep.cpp
                       PRIV_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}
                       PRIV_REQUIRES main rcstage board esp_http_client esp_timer esp_wifi mbedtls freertos espressif__cjson
                                     esp_netif esp_partition spi_flash nvs_flash esp_rom esp_app_format
                       EMBED_FILES ${DOCS}
                       WHOLE_ARCHIVE)
