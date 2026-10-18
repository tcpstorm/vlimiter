// Host-native tests for the pacing arithmetic. Built and run inside the
// Docker build before the .asi is cross-compiled; a regression here fails the
// build.
#include "limiter/pacer.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                               \
		}                                                               \
	} while (0)

int main()
{
	using namespace vlimiter;
	const int64_t hz = 10'000'000; // QueryPerformanceFrequency on Windows 10+

	// interval arithmetic
	{
		CHECK(IntervalTicks(0, hz) == 0);
		CHECK(IntervalTicks(-5, hz) == 0);
		CHECK(IntervalTicks(60, 0) == 0);
		CHECK(IntervalTicks(60, hz) == 166'667);
		CHECK(IntervalTicks(100, hz) == 100'000);
		CHECK(IntervalTicks(144, hz) == 69'444);
		CHECK(IntervalTicks(1000, hz) == 10'000);
	}

	// unlimited: never waits, never remembers
	{
		Pacer p;
		CHECK(p.Arrive(1000, 0) == 1000);
		CHECK(p.Arrive(2000, -1) == 2000);
		CHECK(p.Next() == 0);
	}

	// first frame goes out at once and starts the cadence
	{
		Pacer p;
		const int64_t iv = IntervalTicks(60, hz);
		CHECK(p.Arrive(5'000'000, iv) == 5'000'000);
		CHECK(p.Next() == 5'000'000 + iv);
	}

	// fast frames are held to a fixed cadence, not to "last present + interval"
	{
		Pacer p;
		const int64_t iv = 100'000; // 100 fps
		int64_t now = 1'000'000;
		CHECK(p.Arrive(now, iv) == now);
		// the game renders in 20k ticks each time; presents land on the grid
		for (int i = 1; i <= 10; ++i)
		{
			now += 20'000;
			const int64_t t = p.Arrive(now, iv);
			CHECK(t == 1'000'000 + i * iv);
			now = t; // the hook waits until t, then presents
		}
	}

	// a frame exactly on time does not wait and keeps the grid
	{
		Pacer p;
		const int64_t iv = 100'000;
		CHECK(p.Arrive(0, iv) == 0);
		CHECK(p.Arrive(iv, iv) == iv);
		CHECK(p.Next() == 2 * iv);
	}

	// a late frame presents immediately and restarts the cadence from now
	// (no burst afterwards)
	{
		Pacer p;
		const int64_t iv = 100'000;
		CHECK(p.Arrive(0, iv) == 0);
		CHECK(p.Arrive(350'000, iv) == 350'000); // 3.5 intervals late
		CHECK(p.Next() == 450'000);
		CHECK(p.Arrive(360'000, iv) == 450'000); // next fast frame waits a full interval, not 50k
	}

	// changing the interval mid-flight: the next target uses the new interval
	{
		Pacer p;
		CHECK(p.Arrive(0, 100'000) == 0);
		CHECK(p.Arrive(10'000, 50'000) == 100'000); // already scheduled target stands
		CHECK(p.Next() == 150'000);                 // but the cadence continues at the new interval
	}

	// switching the limit off and on again does not wait for a stale target
	{
		Pacer p;
		CHECK(p.Arrive(0, 100'000) == 0);
		CHECK(p.Arrive(10'000, 0) == 10'000);
		CHECK(p.Arrive(20'000, 100'000) == 20'000);
		CHECK(p.Next() == 120'000);
	}

	// Reset drops the cadence
	{
		Pacer p;
		p.Arrive(0, 100'000);
		p.Reset();
		CHECK(p.Arrive(10'000, 100'000) == 10'000);
	}

	// EMA: primes on the first sample, then moves 1/8 of the way
	{
		Ema e;
		CHECK(!e.Primed());
		e.Add(800);
		CHECK(e.Primed() && e.Value() == 800);
		e.Add(0);
		CHECK(e.Value() == 700);
		e.Add(0, 1);
		CHECK(e.Value() == 350);
		e.Reset();
		CHECK(!e.Primed() && e.Value() == 0);
	}

	if (g_failures)
	{
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("all pacer tests passed\n");
	return 0;
}
