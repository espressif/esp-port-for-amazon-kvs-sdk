idf_component_register(SRCS
                        "signaling_only_main.c"
                        "power_save_handler.c"
                        "trigger_offer_command.c"
                        "query_resolution_command.c"
                        "query_snapshot_command.c"
                        # Vendored from esp_hosted 3.x: CP light sleep ships as
                        # example code, not as part of the component.
                        "cp_light_sleep_controller.c"
                        "host_ps_integration.c"
                    INCLUDE_DIRS ".")
