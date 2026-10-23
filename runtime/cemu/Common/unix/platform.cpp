#include <cstdint>
#include <ctime>

uint32_t GetTickCount()
{
#if defined(__aarch64__) && BOOST_OS_LINUX
	static const uint64_t div = []{ uint64_t f; asm volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f / 1000; }();
	uint64_t t; asm volatile("mrs %0, cntvct_el0" : "=r"(t));
	return (uint32_t)(t / div);
#elif BOOST_OS_LINUX
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (1000 * ts.tv_sec + ts.tv_nsec / 1000000);
#elif BOOST_OS_MACOS
	return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000;
#elif BOOST_OS_BSD
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (1000 * ts.tv_sec + ts.tv_nsec / 1000000);
#endif

}
