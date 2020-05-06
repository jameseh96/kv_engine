add_library(statistics STATIC
        collector.cc
        prometheus.cc
        prometheus_collector.cc
        )

target_include_directories(statistics PUBLIC ${PROMETHEUS_INCLUDE_DIR})

target_link_libraries(statistics hdr_histogram_static ${PROMETHEUS_LIBRARIES})