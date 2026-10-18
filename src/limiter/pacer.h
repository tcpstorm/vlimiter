// pacer.h - the frame-pacing decision, as plain arithmetic on tick counts.
//
// No clock, no sleep, no Windows headers: the render-thread hook feeds it the
// current QueryPerformanceCounter value and the wanted interval, and gets back
// the tick to wait until. That keeps the policy unit-testable on the host
// (tests/test_pacer.cpp) and the hook itself trivial.
//
// Policy: frames are presented on a fixed cadence (next = previous target +
// interval). A frame that arrives late (the game itself took longer than the
// interval) is presented at once and the cadence restarts from now, so a slow
// frame is never followed by a burst of fast ones to "catch up". That is the
// behaviour of the usual external limiters and what feels smooth.
#pragma once

#include <cstdint>

namespace vlimiter
{
	class Pacer
	{
	public:
		// now: current tick. interval: ticks per frame, <= 0 means unlimited.
		// Returns the tick at which this frame may be presented: equal to now
		// when no wait is due, later when the frame arrived early.
		int64_t Arrive(int64_t now, int64_t interval)
		{
			if (interval <= 0)
			{
				m_next = 0;
				return now;
			}
			if (m_next == 0 || now >= m_next)
			{
				// first frame, limit just switched on, or late: no wait, restart the cadence
				m_next = now + interval;
				return now;
			}
			const int64_t target = m_next;
			m_next = target + interval;
			return target;
		}

		// Forget the cadence; the next frame goes out immediately.
		void Reset() { m_next = 0; }

		int64_t Next() const { return m_next; }

	private:
		int64_t m_next = 0;
	};

	// Ticks per frame for an fps limit at a given counter frequency; 0 for
	// "no limit". Rounded to nearest so 1000 Hz / 60 fps is 16.67 -> 17 is
	// avoided: the arithmetic is done in integers at the counter's full
	// resolution (10 MHz on Windows 10 and later).
	inline int64_t IntervalTicks(int fps, int64_t frequency)
	{
		if (fps <= 0 || frequency <= 0)
			return 0;
		return (frequency + fps / 2) / fps;
	}

	// Exponential moving average kept in integer ticks; used for the smoothed
	// frame time the menu shows. alphaShift 3 = weight 1/8 for the new sample.
	class Ema
	{
	public:
		void Add(int64_t sample, int alphaShift = 3)
		{
			if (!m_primed)
			{
				m_value = sample;
				m_primed = true;
				return;
			}
			m_value += (sample - m_value) >> alphaShift;
		}
		int64_t Value() const { return m_value; }
		bool Primed() const { return m_primed; }
		void Reset() { m_value = 0; m_primed = false; }

	private:
		int64_t m_value = 0;
		bool m_primed = false;
	};
}
