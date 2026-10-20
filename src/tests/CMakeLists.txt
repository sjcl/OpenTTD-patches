add_test_files(
    alternating_iterator.cpp
    arena_alloc.cpp
    bitmath_func.cpp
    command.cpp
    enum_over_optimisation.cpp
    flatset_type.cpp
    format_target.cpp
    history_func.cpp
    landscape_partial_pixel_z.cpp
    linkgraph_route_penalties.cpp
    math_func.cpp
    passenger_routing.cpp
    passenger_od.cpp
    passenger_od_assignment.cpp
    passenger_service_graph.cpp
    passenger_service_database.cpp
    passenger_precomputed.cpp
    passenger_async_access.h
    passenger_benchmark_cpu.h
    passenger_service_graph_index.cpp
    passenger_journey_planner.cpp
    passenger_journey_shadow.cpp
    passenger_journey_active.cpp
    passenger_multimodal.cpp
    passenger_test_stations.h
    mock_environment.h
    mock_fontcache.h
    mock_spritecache.cpp
    mock_spritecache.h
    serialisation.cpp
    string_consumer.cpp
    string_func.cpp
    strings_func.cpp
    test_bitset.cpp
    test_main.cpp
    station_cargo_gui.cpp
    station_passenger_route_gui.cpp
    test_map.cpp
    test_network_crypto.cpp
    test_network_debug.cpp
    network_passenger_preparation.h
    test_script_admin.cpp
    test_window_desc.cpp
    tilearea.cpp
    town_auto_fund.cpp
    utf8.cpp
)

# Opt-in allocator instrumentation for the hidden service-index benchmark only.
# It is never linked into the game and is disabled in ordinary test builds.
option(OPTION_SERVICE_INDEX_ALLOCATIONS "Measure service index C++ allocations in openttd_test (MSVC)" OFF)
if(OPTION_SERVICE_INDEX_ALLOCATIONS AND MSVC AND NOT OPTION_NO_SPLIT_LIB)
    target_sources(openttd_test PRIVATE passenger_index_allocations.cpp)
    target_compile_definitions(openttd_test PRIVATE WITH_SERVICE_INDEX_ALLOCATIONS)
endif()
