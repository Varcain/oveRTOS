#include "framework/ove_test.hpp"
#include <cstdio>

/* Stub ove_main for linking (ove_app.c references it) */
extern "C" void ove_main(void)
{
}

int main(void)
{
	int failures = 0;

	printf("=== C++ Mutex Tests ===\n");
	failures += test_cpp_mutex_run();

	printf("=== C++ Recursive Mutex Tests ===\n");
	failures += test_cpp_recursive_mutex_run();

	printf("=== C++ Semaphore Tests ===\n");
	failures += test_cpp_semaphore_run();

	printf("=== C++ Event Tests ===\n");
	failures += test_cpp_event_run();

	printf("=== C++ CondVar Tests ===\n");
	failures += test_cpp_condvar_run();

	printf("=== C++ Queue Tests ===\n");
	failures += test_cpp_queue_run();

	printf("=== C++ Timer Tests ===\n");
	failures += test_cpp_timer_run();

	printf("=== C++ Thread Tests ===\n");
	failures += test_cpp_thread_run();

	printf("=== C++ Thread Stop Tests ===\n");
	failures += test_cpp_thread_stop_run();

	printf("=== C++ EventGroup Tests ===\n");
	failures += test_cpp_eventgroup_run();

	printf("=== C++ LockGuard Tests ===\n");
	failures += test_cpp_lockguard_run();

	printf("=== C++ App Tests ===\n");
	failures += test_cpp_app_run();

	printf("=== C++ Console Tests ===\n");
	failures += test_cpp_console_run();

	printf("=== C++ Time Tests ===\n");
	failures += test_cpp_time_run();

	printf("=== C++ Watchdog Tests ===\n");
	failures += test_cpp_watchdog_run();

	printf("=== C++ NVS Tests ===\n");
	failures += test_cpp_nvs_run();

	printf("=== C++ Shell Tests ===\n");
	failures += test_cpp_shell_run();

	printf("=== C++ BSP Tests ===\n");
	failures += test_cpp_bsp_run();

	printf("=== C++ Board Tests ===\n");
	failures += test_cpp_board_run();

	printf("=== C++ GPIO Tests ===\n");
	failures += test_cpp_gpio_run();

	printf("=== C++ LED Tests ===\n");
	failures += test_cpp_led_run();

	printf("=== C++ Audio Tests ===\n");
	failures += test_cpp_audio_run();

	printf("=== C++ Filesystem Tests ===\n");
	failures += test_cpp_fs_run();

	printf("=== C++ Stream Tests ===\n");
	failures += test_cpp_stream_run();

	printf("=== C++ Workqueue Tests ===\n");
	failures += test_cpp_workqueue_run();

	printf("=== C++ Static Init Tests ===\n");
	failures += test_cpp_static_init_run();

	printf("=== C++ Infer Tests ===\n");
	failures += test_cpp_infer_run();

	printf("=== ove::cpp::Error Tests ===\n");
	failures += test_cppns_error_run();

	printf("\n=== Summary: %d test group(s) had failures ===\n", failures);
	return failures ? 1 : 0;
}
