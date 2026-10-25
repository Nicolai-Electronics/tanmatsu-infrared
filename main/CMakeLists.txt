idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.c"
		"app.c"
		"catt_ir.c"
		"code_db.c"
		"ir_nec_encoder.c"
		"ui.c"
	PRIV_REQUIRES
		esp_lcd
		esp_driver_rmt
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
		tanmatsu-infrared
		cjson
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
