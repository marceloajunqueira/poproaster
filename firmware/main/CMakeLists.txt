idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        board_config
        roast_core
        safety
        roaster_hal
        storage
        ui_display
        web_api
        # TODO(T047): re-add artisan_adapter once that component is implemented.
)
