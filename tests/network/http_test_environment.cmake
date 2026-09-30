# GoogleTest discovers each case as a separate process. Configure the active path
# only for that case; configuring the whole executable would skip inactive tests.
set_tests_properties(HostOverride.ActiveExactHostMatchFromJsonFile PROPERTIES
    ENVIRONMENT "SHADPS4_HTTP_HOST_OVERRIDES_JSON=${CMAKE_CURRENT_LIST_DIR}/host_overrides_fixture.json"
)
