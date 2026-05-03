PROJECT_NAME = "oveRTOS C++ API" PROJECT_BRIEF = "C++20 RAII wrappers for the oveRTOS C API" INPUT =
	bindings / cpp / ove docs - site / doxygen / doxygen - cpp -
	mainpage.md USE_MDFILE_AS_MAINPAGE =
		docs - site / doxygen / doxygen - cpp -
		mainpage.md
			RECURSIVE = YES FILE_PATTERNS = *.hpp EXTENSION_MAPPING = hpp = C++ OUTPUT_DIRECTORY =
			output / docs / doxygen -
			cpp GENERATE_HTML = YES GENERATE_XML = YES GENERATE_LATEX = NO HTML_OUTPUT = html EXTRACT_ALL = NO EXTRACT_STATIC =
				YES OPTIMIZE_OUTPUT_FOR_C = NO SORT_MEMBER_DOCS = NO ENABLE_PREPROCESSING = YES MACRO_EXPANSION = YES
#Heap - mode toggles are static — not Kconfig - derived.
#CONFIG_OVE_ *symbols are auto - generated into Doxyfile.predefined by
#scripts / kconfig - doxyfile - gen.py(re - run by `make docs`).
					PREDEFINED = OVE_HEAP_THREAD = 1 OVE_HEAP_MUTEX = 1 OVE_HEAP_SEM =
						1 OVE_HEAP_EVENT = 1 OVE_HEAP_CONDVAR = 1 OVE_HEAP_QUEUE =
							1 OVE_HEAP_TIMER = 1 OVE_HEAP_STREAM = 1 OVE_HEAP_EVENTGROUP =
								1 OVE_HEAP_WORKQUEUE = 1 OVE_HEAP_FS = 1 OVE_HEAP_WATCHDOG =
									1 OVE_HEAP_SYNC = 1 OVE_HEAP_UART = 1 OVE_HEAP_SPI =
										1 OVE_HEAP_I2C = 1 OVE_HEAP_I2S = 1 OVE_HEAP_INFER =
											1 OVE_HEAP_NET = 1 OVE_HEAP_NET_TLS =
												1 OVE_HEAP_NET_HTTP =
													1 OVE_HEAP_NET_MQTT =
														1
	@INCLUDE = output / docs / Doxyfile.predefined EXPAND_ONLY_PREDEF =
			   YES WARN_IF_UNDOCUMENTED = YES WARN_IF_DOC_ERROR = YES
