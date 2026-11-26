idf_component_register(SRCS "relay_manager.c"
                       INCLUDE_DIRS "."
                       REQUIRES uart_driver holtek_protocol bt)
