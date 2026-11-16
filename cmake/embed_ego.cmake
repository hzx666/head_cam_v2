configure_file("${SDK}" "${OUTPUT}.sdk" COPYONLY)
execute_process(COMMAND "${STRIP}" --strip-unneeded "${HELPER}" "${OUTPUT}.sdk" COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${OUTPUT}" ".section .rodata\n.global ego_helper_start,ego_helper_end,ego_library_start,ego_library_end\nego_helper_start:\n.incbin \"${HELPER}\"\nego_helper_end:\nego_library_start:\n.incbin \"${OUTPUT}.sdk\"\nego_library_end:\n.section .note.GNU-stack,\"\",%progbits\n")
