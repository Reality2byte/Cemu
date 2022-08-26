#include "Cafe/HW/Espresso/Const.h"
#include <immintrin.h>
#include "asm/x64util.h"
#include "config/ActiveSettings.h"
#include "util/helpers/fspinlock.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS
static __inline__
unsigned __int64 _umul128(unsigned __int64,
                          unsigned __int64,
                          unsigned __int64*);
#endif

uint64 _rdtscLastMeasure = 0;
uint64 _rdtscFrequency = 0;

struct uint128_t
{
	uint64 low;
	uint64 high;
};

static_assert(sizeof(uint128_t) == 16);

uint128_t _rdtscAcc{};

#pragma intrinsic(__rdtsc)

uint64 muldiv64(uint64 a, uint64 b, uint64 d)
{
	uint64 diva = a / d;
	uint64 moda = a % d;
	uint64 divb = b / d;
	uint64 modb = b % d;
	return diva * b + moda * divb + moda * modb / d;
}

bool PPCTimer_hasInvariantRDTSCSupport()
{
	uint32 cpuv[4];
	__cpuid((int*)cpuv, 0x80000007);
	return ((cpuv[3] >> 8) & 1);
}

uint64 PPCTimer_estimateRDTSCFrequency()
{
	if (PPCTimer_hasInvariantRDTSCSupport() == false)
		forceLog_printf("Invariant TSC not supported");

	_mm_mfence();
	unsigned __int64 tscStart = __rdtsc();
	unsigned int startTime = GetTickCount();
	HRTick startTick = HighResolutionTimer::now().getTick();
	// wait roughly 3 seconds
	while (true)
	{
		if ((GetTickCount() - startTime) >= 3000)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	_mm_mfence();
	HRTick stopTick = HighResolutionTimer::now().getTick();
	unsigned __int64 tscEnd = __rdtsc();
	// derive frequency approximation from measured time difference
	uint64 tsc_diff = tscEnd - tscStart;
	uint64 hrtFreq = 0;
	uint64 hrtDiff = HighResolutionTimer::getTimeDiffEx(startTick, stopTick, hrtFreq);
	uint64 tsc_freq = muldiv64(tsc_diff, hrtFreq, hrtDiff);

	// uint64 freqMultiplier = tsc_freq / hrtFreq;
	//forceLog_printf("RDTSC measurement test:");
	//forceLog_printf("TSC-diff:   0x%016llx", tsc_diff);
	//forceLog_printf("TSC-freq:   0x%016llx", tsc_freq);
	//forceLog_printf("HPC-diff:   0x%016llx", qpc_diff);
	//forceLog_printf("HPC-freq:   0x%016llx", (uint64)qpc_freq.QuadPart);
	//forceLog_printf("Multiplier: 0x%016llx", freqMultiplier);

	return tsc_freq;
}

int PPCTimer_initThread()
{
	_rdtscFrequency = PPCTimer_estimateRDTSCFrequency();
	return 0;
}

void PPCTimer_init()
{
	std::thread t(PPCTimer_initThread);
	t.detach();
	_rdtscLastMeasure = __rdtsc();
}

uint64 _tickSummary = 0;

void PPCTimer_start()
{
	_rdtscLastMeasure = __rdtsc();
	_tickSummary = 0;
}

uint64 PPCTimer_getRawTsc()
{
	return __rdtsc();
}

uint64 PPCTimer_microsecondsToTsc(uint64 us)
{
	return (us * _rdtscFrequency) / 1000000ULL;
}

uint64 PPCTimer_tscToMicroseconds(uint64 us)
{
	uint128_t r{};
	#if BOOST_OS_WINDOWS
	r.low = _umul128(us, 1000000ULL, &r.high);
	#else
	r.low = _umul128(us, 1000000ULL, (unsigned long long*)&r.high);
	#endif


	uint64 remainder;
#if _MSC_VER < 1923
	const uint64 microseconds = udiv128(r.low, r.high, _rdtscFrequency, &remainder);
#else
	const uint64 microseconds = _udiv128(r.high, r.low, _rdtscFrequency, &remainder);
#endif

	return microseconds;
}

bool PPCTimer_isReady()
{
	return _rdtscFrequency != 0;
}

void PPCTimer_waitForInit()
{
	while (!PPCTimer_isReady()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

FSpinlock sTimerSpinlock;

// thread safe
uint64 PPCTimer_getFromRDTSC()
{
	sTimerSpinlock.acquire();
	_mm_mfence();
	uint64 rdtscCurrentMeasure = __rdtsc();
	uint64 rdtscDif = rdtscCurrentMeasure - _rdtscLastMeasure;
	// optimized max(rdtscDif, 0) without conditionals
	rdtscDif = rdtscDif & ~(uint64)((sint64)rdtscDif >> 63);

	uint128_t diff{};
#if BOOST_OS_WINDOWS
	diff.low = _umul128(rdtscDif, Espresso::CORE_CLOCK, &diff.high);
#else
	diff.low = _umul128(rdtscDif, Espresso::CORE_CLOCK, (unsigned long long*)&diff.high);
#endif

	if(rdtscCurrentMeasure > _rdtscLastMeasure)
		_rdtscLastMeasure = rdtscCurrentMeasure; // only travel forward in time

	uint8 c = 0;
	#if BOOST_OS_WINDOWS
	c = _addcarry_u64(c, _rdtscAcc.low, diff.low, &_rdtscAcc.low);
	_addcarry_u64(c, _rdtscAcc.high, diff.high, &_rdtscAcc.high);
	#else
	// requires casting because of long / long long nonesense
	c = _addcarry_u64(c, _rdtscAcc.low, diff.low, (unsigned long long*)&_rdtscAcc.low);
	_addcarry_u64(c, _rdtscAcc.high, diff.high, (unsigned long long*)&_rdtscAcc.high);
	#endif

	uint64 remainder;
#if _MSC_VER < 1923
	 uint64 elapsedTick = udiv128(_rdtscAcc.low, _rdtscAcc.high, _rdtscFrequency, &remainder);
#else
	 uint64 elapsedTick = _udiv128(_rdtscAcc.high, _rdtscAcc.low, _rdtscFrequency, &remainder);
#endif

	_rdtscAcc.low = remainder;
	_rdtscAcc.high = 0;

	// timer scaling
	elapsedTick <<= 3ull; // *8
	uint8 timerShiftFactor = ActiveSettings::GetTimerShiftFactor();
	elapsedTick >>= timerShiftFactor;

	_tickSummary += elapsedTick;

	sTimerSpinlock.release();
	return _tickSummary;
}