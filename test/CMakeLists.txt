
find_package(Catch2 REQUIRED)
enable_testing()

add_executable(unit_tests
        main.cpp
        cycles.cpp
        double_edges.cpp)

target_compile_features(unit_tests PUBLIC cxx_std_17)

target_link_libraries(
        unit_tests
        PRIVATE Catch2::Catch2)

target_include_directories(unit_tests
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>)

include(CTest)
include(ParseAndAddCatchTests)
ParseAndAddCatchTests(unit_tests)

add_test(NAME unit COMMAND ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}/unit_tests)