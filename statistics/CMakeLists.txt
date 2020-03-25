add_library(statistics STATIC
        collector.cc
        prometheus_collector.cc
        )

target_link_libraries(statistics hdr_histogram_static)