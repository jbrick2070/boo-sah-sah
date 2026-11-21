# Use the actual process exit status: a driver failure during final destruction
# can occur after the app has successfully written its runtime diagnostics.
file(MAKE_DIRECTORY "${OUT}")
get_filename_component(app_dir "${APP}" DIRECTORY)
set(report "${OUT}/diagnostics.json")
set(log "${OUT}/playback.log")
execute_process(
    COMMAND "${APP}" --force-warp --preset safe --disable-npu --effects-detail high
            --reduced-flash --reduced-motion --lang zh-TW --mute --no-activate
            --start-seconds 258 --duration-seconds 4
            --export-diagnostics "${report}" --log-file "${log}"
    WORKING_DIRECTORY "${app_dir}" TIMEOUT 75 RESULT_VARIABLE exit_status)
if(NOT exit_status STREQUAL "0")
    message(FATAL_ERROR "WARP diagnostic process exit: ${exit_status}")
endif()
file(READ "${report}" data)
string(JSON backend GET "${data}" engines RT diagnostic_code)
string(JSON accelerated GET "${data}" engines RT successfully_executed_test_work)
string(JSON frames GET "${data}" runtime rendered_frames)
string(JSON timings GET "${data}" runtime gpu_frametime_ms sample_count)
string(JSON faults GET "${data}" runtime effect_fault_count)
if(NOT backend STREQUAL "WARP_PROCEDURAL_DIAGNOSTIC" OR accelerated OR
   frames LESS 3 OR timings LESS 1 OR faults GREATER 0)
    message(FATAL_ERROR "WARP failed to retire frames: ${backend}, frames=${frames}, timings=${timings}, faults=${faults}")
endif()
file(READ "${log}" messages)
if(messages MATCHES "GPU teardown drain failed")
    message(FATAL_ERROR "WARP failed to drain submitted work")
endif()
