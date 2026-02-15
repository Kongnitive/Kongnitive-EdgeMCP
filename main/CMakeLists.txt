idf_component_register(SRCS "wifi_manager.c" "mcp_server.c" "mcp_tools.c" "mcp_protocol.c" "jsonrpc.c" "main.c" "keep_alive.c"
                            "mcp_log.c" "mcp_ota.c" "lua_runtime.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_https_server nvs_flash esp_timer esp_netif esp_eth esp_wifi
                                  esp_http_client app_update json esp_driver_gpio esp_driver_i2c
                                  spiffs lua
                    EMBED_TXTFILES "certs/servercert.pem"
                                   "certs/prvtkey.pem")
