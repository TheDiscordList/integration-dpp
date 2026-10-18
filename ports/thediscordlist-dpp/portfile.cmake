vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO TheDiscordList/integration-dpp
    REF "v${VERSION}"
    SHA512 ee72f5255f7e8f8651c9743ac05e43f91e73ba39feb1b3f98eb91f211d3a2185feca692822df7a97f91218088ce5b1284fb64fb0ad80054626163d026b14c092
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DTHEDISCORDLIST_DPP_BUILD_TESTS=OFF
        -DTHEDISCORDLIST_DPP_INSTALL=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/thediscordlist-dpp)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
