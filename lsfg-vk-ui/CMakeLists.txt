find_package(Qt6 REQUIRED COMPONENTS Quick)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

set(UI_SOURCES
    "src/backend.cpp"
    "src/main.cpp"
    "src/utils.cpp")

set(UI_QML_FILES
    "rsc/dialogs/CenteredDialog.qml"
    "rsc/dialogs/LargeDialog.qml"
    "rsc/panes/Group.qml"
    "rsc/panes/GroupEntry.qml"
    "rsc/panes/Pane.qml"
    "rsc/widgets/FileEdit.qml"
    "rsc/widgets/FlowSlider.qml"
    "rsc/widgets/List.qml"
    "rsc/UI.qml")

set(UI_RESOURCES
    "rsc/gay.pancake.lsfg-vk-ui.png")

qt_add_executable(lsfg-vk-ui ${UI_SOURCES})

set_target_properties(lsfg-vk-ui PROPERTIES
    AUTOMOC ON
    AUTOUIC ON)

qt_add_qml_module(lsfg-vk-ui
    URI LsfgVkUi
    VERSION 1.0
    RESOURCE_PREFIX /
    NO_RESOURCE_TARGET_PATH
    NO_IMPORT_SCAN
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lsfg-vk-ui-qml
    QML_FILES ${UI_QML_FILES}
    RESOURCES ${UI_RESOURCES})

target_compile_options(lsfg-vk-ui PRIVATE # QT-codegen warnings
    -Wno-unknown-warning-option
    -Wno-ctad-maybe-unsupported
    -Wno-unsafe-buffer-usage-in-libc-call
    -Wno-global-constructors
    -Wno-unsafe-buffer-usage)

target_link_libraries(lsfg-vk-ui
    PRIVATE lsfg-vk-common
    PRIVATE lsfg-vk-backend
    PRIVATE Qt6::Quick)

install(TARGETS lsfg-vk-ui
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(LSFGVK_INSTALL_XDG_FILES)
    install(FILES "rsc/gay.pancake.lsfg-vk-ui.png"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps")
    install(FILES "rsc/gay.pancake.lsfg-vk-ui.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
endif()
