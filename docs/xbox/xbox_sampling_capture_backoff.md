# Back off repeated slow sampling captures

The in-process profiler suspends a named thread, reads its context, copies its
stack and resumes it before unwinding. Some Xbox captures took tens of
milliseconds or longer. The whole Capture elapsed time does not establish how
long the target was actually held, or prove the profiler caused a gameplay hitch.

After Capture takes more than 50 ms, the sampler now rests that target for 1, 2,
4 and at most 8 seconds on consecutive slow captures. A fast capture resets this
backoff. Normal and stalled-frame samples use the same gate; other targets remain
eligible. Policy updates, counters and logging run after the target is resumed.
The policy cannot cancel or time-bound an already running GetThreadContext call,
and it does not guarantee a bound on Stop latency.

Skipped attempts and failed captures are not counted as recorded running/waiting
samples. Window and thread records disclose skipped/slow captures. Thread CPU
totals still span the full window, but stack proportions can be biased by missing
samples; the report warns about this coverage loss.

Legacy `suspended_us_*` fields remain readable. The report describes them as
Capture elapsed time and computes the average weighted by each window's capture
count, instead of averaging window averages. Neither the field nor this change
measures exact target-held time. This draft has not been tested on Xbox and does
not claim better gameplay FPS.

## Validation

From a Windows x64 MSVC Developer prompt at the repository root:

```bat
mkdir build-uwp\sampling-capture-backoff
cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\sampling-capture-backoff.cpp /Fobuild-uwp\sampling-capture-backoff\ /Febuild-uwp\sampling-capture-backoff\gate.exe
build-uwp\sampling-capture-backoff\gate.exe
python tools\xbox\tests\profile-report-capture.py
```

The policy gate covers threshold/deadline boundaries, escalation, cap, recovery,
independent targets and overflow. The report fixture checks weighted averages,
legacy/new records and zero captures. The existing `sampling-profiler.cpp` native
gate also passed against this source (busy/deep/fiber/allocator/blocked threads,
JIT caller/thunk attribution). A separate test-only delay injected after
ResumeThread exercised the cooldown/coverage path without keeping the target
suspended during the delay; it does not reproduce the Xbox kernel's behavior.

## References

- [Microsoft GetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadcontext)
- [Microsoft SuspendThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-suspendthread)

Keep allocation, logging and additional locks outside the suspended interval:
the target can already own locks needed by the sampler.
