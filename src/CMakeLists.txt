#
# asterisk-chan-quectel
#

INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/source-files.cmake)

ADD_LIBRARY(asterisk-chan-quectel SHARED ${SOURCES} ${HEADERS})
TARGET_COMPILE_FEATURES(asterisk-chan-quectel PUBLIC c_std_17)

TARGET_INCLUDE_DIRECTORIES(asterisk-chan-quectel BEFORE PRIVATE ${CMAKE_BINARY_DIR}/include)

TARGET_COMPILE_DEFINITIONS(asterisk-chan-quectel PRIVATE 
    _GNU_SOURCE
    AST_MODULE_SELF_SYM=__internal_chan_quectel_self
    HAVE_CONFIG_H
)

TARGET_LINK_LIBRARIES(asterisk-chan-quectel PRIVATE
    Threads::Threads
    ALSA::ALSA
    SQLite::SQLite3
    Iconv::Iconv
)

SET_TARGET_PROPERTIES(asterisk-chan-quectel PROPERTIES
    LIBRARY_OUTPUT_NAME ${AST_MODULE}
    PREFIX ""
)

TARGET_COMPILE_OPTIONS(asterisk-chan-quectel PRIVATE
    $<$<AND:$<C_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,8>>:-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=bin -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=src>
    $<$<AND:$<C_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<C_COMPILER_VERSION>,4>>:-Wall>
)

INSTALL(
    TARGETS asterisk-chan-quectel
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/asterisk/modules
    COMPONENT chan-quectel
)

# install configuration file
INSTALL(
    FILES ${CMAKE_SOURCE_DIR}/quectel.conf
    DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/asterisk
    COMPONENT chan-quectel
)

# formatting targets
IF(NOT CLANG_FORMAT OR "${CLANG_FORMAT}" STREQUAL CLANG_FORMAT-NOTFOUND)
    MESSAGE(WARNING "Cannot create formatting targets - clang-format not found")
ELSE()
    ADD_CUSTOM_TARGET(asterisk-chan-quectel-code-formatter
        COMMAND ${CLANG_FORMAT}
            -i
            --style=file
            $<TARGET_PROPERTY:asterisk-chan-quectel,SOURCES>
        WORKING_DIRECTORY
            $<TARGET_PROPERTY:asterisk-chan-quectel,SOURCE_DIR>
        COMMENT
            "Formatting code"
        COMMAND_EXPAND_LISTS
    )

    ADD_CUSTOM_TARGET(asterisk-chan-quectel-code-formatting-check
        COMMAND ${CLANG_FORMAT}
            --dry-run
            --verbose
            -i
            --style=file
            $<TARGET_PROPERTY:asterisk-chan-quectel,SOURCES>
        WORKING_DIRECTORY
            $<TARGET_PROPERTY:asterisk-chan-quectel,SOURCE_DIR>
        COMMENT
            "Checking code formatting"
        COMMAND_EXPAND_LISTS
    )

    # ADD_DEPENDENCIES(asterisk-chan-quectel asterisk-chan-quectel-code-formatter)
    # ADD_DEPENDENCIES(asterisk-chan-quectel asterisk-chan-quectel-code-formatting-check)
ENDIF()
