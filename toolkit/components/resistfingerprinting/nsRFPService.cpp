/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsRFPService.h"

#include <algorithm>
#include <cfloat>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <type_traits>
#include <utility>

#include "MainThreadUtils.h"
#include "ScopedNSSTypes.h"

#include "mozilla/ArrayIterator.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Casting.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/glean/GleanMetrics.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/HelperMacros.h"
#include "mozilla/Likely.h"
#include "mozilla/Logging.h"
#include "mozilla/MacroForEach.h"
#include "mozilla/OriginAttributes.h"
#include "mozilla/Preferences.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_javascript.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/StaticPrefs_privacy.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TextEvents.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/KeyboardEventBinding.h"
#include "mozilla/fallible.h"
#include "mozilla/XorShift128PlusRNG.h"

#include "nsBaseHashtable.h"
#include "nsComponentManagerUtils.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsCoord.h"
#include "nsTHashMap.h"
#include "nsDebug.h"
#include "nsError.h"
#include "nsHashKeys.h"
#include "nsJSUtils.h"
#include "nsLiteralString.h"
#include "nsPrintfCString.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsStringFlags.h"
#include "nsTArray.h"
#include "nsTLiteralString.h"
#include "nsTPromiseFlatString.h"
#include "nsTStringRepr.h"
#include "nsXPCOM.h"

#include "nsICookieJarSettings.h"
#include "nsICryptoHash.h"
#include "nsIGlobalObject.h"
#include "nsIObserverService.h"
#include "nsIRandomGenerator.h"
#include "nsIUserIdleService.h"
#include "nsIXULAppInfo.h"

#include "nscore.h"
#include "prenv.h"
#include "prtime.h"
#include "xpcpublic.h"

#include "js/Date.h"

using namespace mozilla;

static mozilla::LazyLogModule gResistFingerprintingLog(
    "nsResistFingerprinting");

#define RESIST_FINGERPRINTINGPROTECTION_OVERRIDE_PREF \
  "privacy.fingerprintingProtection.overrides"
#define RFP_TIMER_UNCONDITIONAL_VALUE 20
#define LAST_PB_SESSION_EXITED_TOPIC "last-pb-context-exited"

static constexpr uint32_t kVideoFramesPerSec = 30;
static constexpr uint32_t kVideoDroppedRatio = 5;

#define RFP_DEFAULT_SPOOFING_KEYBOARD_LANG KeyboardLang::EN
#define RFP_DEFAULT_SPOOFING_KEYBOARD_REGION KeyboardRegion::US

// Fingerprinting protections that are enabled by default. This can be
// overridden using the privacy.fingerprintingProtection.overrides pref.
const uint64_t kDefaultFingerintingProtections =
    uint64_t(RFPTarget::CanvasRandomization) |
    uint64_t(RFPTarget::FontVisibilityLangPack);

// ============================================================================
// ============================================================================
// ============================================================================
// Structural Stuff & Pref Observing

NS_IMPL_ISUPPORTS(nsRFPService, nsIObserver)

static StaticRefPtr<nsRFPService> sRFPService;
static bool sInitialized = false;

// Actually enabled fingerprinting protections.
static Atomic<uint64_t> sEnabledFingerintingProtections;

/* static */
nsRFPService* nsRFPService::GetOrCreate() {
  if (!sInitialized) {
    sRFPService = new nsRFPService();
    nsresult rv = sRFPService->Init();

    if (NS_FAILED(rv)) {
      sRFPService = nullptr;
      return nullptr;
    }

    ClearOnShutdown(&sRFPService);
    sInitialized = true;
  }

  return sRFPService;
}

static const char* gCallbackPrefs[] = {
    RESIST_FINGERPRINTINGPROTECTION_OVERRIDE_PREF,
    nullptr,
};

nsresult nsRFPService::Init() {
  MOZ_ASSERT(NS_IsMainThread());

  nsresult rv;

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  NS_ENSURE_TRUE(obs, NS_ERROR_NOT_AVAILABLE);

  rv = obs->AddObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID, false);
  NS_ENSURE_SUCCESS(rv, rv);

  if (XRE_IsParentProcess()) {
    rv = obs->AddObserver(this, LAST_PB_SESSION_EXITED_TOPIC, false);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = obs->AddObserver(this, OBSERVER_TOPIC_IDLE_DAILY, false);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  Preferences::RegisterCallbacks(nsRFPService::PrefChanged, gCallbackPrefs,
                                 this);

  JS::SetReduceMicrosecondTimePrecisionCallback(
      nsRFPService::ReduceTimePrecisionAsUSecsWrapper);

  // Called from here to get the initial list of enabled fingerprinting
  // protections.
  UpdateFPPOverrideList();

  return rv;
}

/* static */
bool nsRFPService::IsRFPPrefEnabled(bool aIsPrivateMode) {
  if (StaticPrefs::privacy_resistFingerprinting_DoNotUseDirectly() ||
      (aIsPrivateMode &&
       StaticPrefs::privacy_resistFingerprinting_pbmode_DoNotUseDirectly())) {
    return true;
  }
  return false;
}

/* static */
bool nsRFPService::IsRFPEnabledFor(RFPTarget aTarget) {
  MOZ_ASSERT(aTarget != RFPTarget::AllTargets);

  if (StaticPrefs::privacy_resistFingerprinting_DoNotUseDirectly() ||
      StaticPrefs::privacy_resistFingerprinting_pbmode_DoNotUseDirectly()) {
    return true;
  }

  if (StaticPrefs::privacy_fingerprintingProtection_DoNotUseDirectly() ||
      StaticPrefs::privacy_fingerprintingProtection_pbmode_DoNotUseDirectly()) {
    if (aTarget == RFPTarget::IsAlwaysEnabledForPrecompute) {
      return true;
    }
    return sEnabledFingerintingProtections & uint64_t(aTarget);
  }

  return false;
}

void nsRFPService::UpdateFPPOverrideList() {
  nsAutoString targetOverrides;
  nsresult rv = Preferences::GetString(
      RESIST_FINGERPRINTINGPROTECTION_OVERRIDE_PREF, targetOverrides);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    MOZ_LOG(gResistFingerprintingLog, LogLevel::Warning,
            ("Could not get fingerprinting override pref value"));
    return;
  }

  uint64_t enabled = kDefaultFingerintingProtections;
  for (const nsAString& each : targetOverrides.Split(',')) {
    Maybe<RFPTarget> mappedValue =
        nsRFPService::TextToRFPTarget(Substring(each, 1, each.Length() - 1));
    if (mappedValue.isSome()) {
      RFPTarget target = mappedValue.value();
      if (target == RFPTarget::IsAlwaysEnabledForPrecompute) {
        MOZ_LOG(gResistFingerprintingLog, LogLevel::Warning,
                ("RFPTarget::%s is not a valid value",
                 NS_ConvertUTF16toUTF8(each).get()));
      } else if (each[0] == '+') {
        enabled |= uint64_t(target);
        MOZ_LOG(gResistFingerprintingLog, LogLevel::Warning,
                ("Mapped value %s (0x%" PRIx64
                 "), to an addition, now we have 0x%" PRIx64,
                 NS_ConvertUTF16toUTF8(each).get(), uint64_t(target), enabled));
      } else if (each[0] == '-') {
        enabled &= ~uint64_t(target);
        MOZ_LOG(gResistFingerprintingLog, LogLevel::Warning,
                ("Mapped value %s (0x%" PRIx64
                 ") to a subtraction, now we have 0x%" PRIx64,
                 NS_ConvertUTF16toUTF8(each).get(), uint64_t(target), enabled));
      } else {
        MOZ_LOG(gResistFingerprintingLog, LogLevel::Warning,
                ("Mapped value %s (0x%" PRIx64
                 ") to an RFPTarget Enum, but the first "
                 "character wasn't + or -",
                 NS_ConvertUTF16toUTF8(each).get(), uint64_t(target)));
      }
    } else {
      MOZ_LOG(gResistFingerprintingLog, LogLevel::Warning,
              ("Could not map the value %s to an RFPTarget Enum",
               NS_ConvertUTF16toUTF8(each).get()));
    }
  }

  sEnabledFingerintingProtections = enabled;
}

/* static */
Maybe<RFPTarget> nsRFPService::TextToRFPTarget(const nsAString& aText) {
#define ITEM_VALUE(name, value)     \
  if (aText.EqualsLiteral(#name)) { \
    return Some(RFPTarget::name);   \
  }

#include "RFPTargets.inc"
#undef ITEM_VALUE

  return Nothing();
}

void nsRFPService::StartShutdown() {
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();

  if (obs) {
    obs->RemoveObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID);
    if (XRE_IsParentProcess()) {
      obs->RemoveObserver(this, LAST_PB_SESSION_EXITED_TOPIC);
      obs->RemoveObserver(this, OBSERVER_TOPIC_IDLE_DAILY);
    }
  }

  Preferences::UnregisterCallbacks(nsRFPService::PrefChanged, gCallbackPrefs,
                                   this);
}

// static
void nsRFPService::PrefChanged(const char* aPref, void* aSelf) {
  static_cast<nsRFPService*>(aSelf)->PrefChanged(aPref);
}

void nsRFPService::PrefChanged(const char* aPref) {
  nsDependentCString pref(aPref);

  if (pref.EqualsLiteral(RESIST_FINGERPRINTINGPROTECTION_OVERRIDE_PREF)) {
    UpdateFPPOverrideList();
  }
}

NS_IMETHODIMP
nsRFPService::Observe(nsISupports* aObject, const char* aTopic,
                      const char16_t* aMessage) {
  if (strcmp(NS_XPCOM_SHUTDOWN_OBSERVER_ID, aTopic) == 0) {
    StartShutdown();
  }

  if (strcmp(LAST_PB_SESSION_EXITED_TOPIC, aTopic) == 0) {
    // Clear the private session key when the private session ends so that we
    // can generate a new key for the new private session.
    ClearSessionKey(true);
  }

  if (!strcmp(OBSERVER_TOPIC_IDLE_DAILY, aTopic)) {
    if (StaticPrefs::
            privacy_resistFingerprinting_randomization_daily_reset_enabled()) {
      ClearSessionKey(false);
    }

    if (StaticPrefs::
            privacy_resistFingerprinting_randomization_daily_reset_private_enabled()) {
      ClearSessionKey(true);
    }
  }

  return NS_OK;
}

// ============================================================================
// ============================================================================
// ============================================================================
// Reduce Timer Precision Stuff

constexpr double RFP_TIME_ATOM_MS = 16.667;  // 60Hz, 1000/60 but rounded.
/*
In RFP RAF always runs at 60Hz, so we're ~0.02% off of 1000/60 here.
```js
extra_frames_per_frame = 16.667 / (1000/60) - 1 // 0.00028
sec_per_extra_frame = 1 / (extra_frames_per_frame * 60) // 833.33
min_per_extra_frame = sec_per_extra_frame / 60 // 13.89
```
We expect an extra frame every ~14 minutes, which is enough to be smooth.
16.67 would be ~1.4 minutes, which is OK, but is more noticable.
Put another way, if this is the only unacceptable hitch you have across 14
minutes, I'm impressed, and we might revisit this.
*/

/* static */
double nsRFPService::TimerResolution(RTPCallerType aRTPCallerType) {
  double prefValue = StaticPrefs::
      privacy_resistFingerprinting_reduceTimerPrecision_microseconds();
  if (aRTPCallerType == RTPCallerType::ResistFingerprinting) {
    return std::max(RFP_TIME_ATOM_MS * 1000.0, prefValue);
  }
  return prefValue;
}

/**
 * The purpose of this function is to deterministicly generate a random midpoint
 * between a lower clamped value and an upper clamped value. Assuming a clamping
 * resolution of 100, here is an example:
 *
 * |---------------------------------------|--------------------------|
 * lower clamped value (e.g. 300)          |           upper clamped value (400)
 *                              random midpoint (e.g. 360)
 *
 * If our actual timestamp (e.g. 325) is below the midpoint, we keep it clamped
 * downwards. If it were equal to or above the midpoint (e.g. 365) we would
 * round it upwards to the largest clamped value (in this example: 400).
 *
 * The question is: does time go backwards?
 *
 * The midpoint is deterministicly random and generated from three components:
 * a secret seed, a per-timeline (context) 'mix-in', and a clamped time.
 *
 * When comparing times across different seed values: time may go backwards.
 * For a clamped time of 300, one seed may generate a midpoint of 305 and
 * another 395. So comparing an (actual) timestamp of 325 and 351 could see the
 * 325 clamped up to 400 and the 351 clamped down to 300. The seed is
 * per-process, so this case occurs when one can compare timestamps
 * cross-process. This is uncommon (because we don't have site isolation.) The
 * circumstances this could occur are BroadcastChannel, Storage Notification,
 * and in theory (but not yet implemented) SharedWorker. This should be an
 * exhaustive list (at time of comment writing!).
 *
 * Aside from cross-process communication, derived timestamps across different
 * time origins may go backwards. (Specifically, derived means adding two
 * timestamps together to get an (approximate) absolute time.)
 * Assume a page and a worker. If one calls performance.now() in the page and
 * then triggers a call to performance.now() in the worker, the following
 * invariant should hold true:
 *             page.performance.timeOrigin + page.performance.now() <
 *                      worker.performance.timeOrigin + worker.performance.now()
 *
 * We break this invariant.
 *
 * The 'Context Mix-in' is a securely generated random seed that is unique for
 * each timeline that starts over at zero. It is needed to ensure that the
 * sequence of midpoints (as calculated by the secret seed and clamped time)
 * does not repeat. In RelativeTimeline.h, we define a 'RelativeTimeline' class
 * that can be inherited by any object that has a relative timeline. The most
 * obvious examples are Documents and Workers. An attacker could let time go
 * forward and observe (roughly) where the random midpoints fall. Then they
 * create a new object, time starts back over at zero, and they know
 * (approximately) where the random midpoints are.
 *
 * When the timestamp given is a non-relative timestamp (e.g. it is relative to
 * the unix epoch) it is not possible to replay a sequence of random values.
 * Thus, providing a zero context pointer is an indicator that the timestamp
 * given is absolute and does not need any additional randomness.
 *
 * @param aClampedTimeUSec [in]  The clamped input time in microseconds.
 * @param aResolutionUSec  [in]  The current resolution for clamping in
 *                               microseconds.
 * @param aMidpointOut     [out] The midpoint, in microseconds, between [0,
 *                               aResolutionUSec].
 * @param aContextMixin    [in]  An opaque random value for relative
 *                               timestamps. 0 for absolute timestamps
 * @param aSecretSeed      [in]  TESTING ONLY. When provided, the current seed
 *                               will be replaced with this value.
 * @return                 A nsresult indicating success of failure. If the
 *                         function failed, nothing is written to aMidpointOut
 */

/* static */
nsresult nsRFPService::RandomMidpoint(long long aClampedTimeUSec,
                                      long long aResolutionUSec,
                                      int64_t aContextMixin,
                                      long long* aMidpointOut,
                                      uint8_t* aSecretSeed /* = nullptr */) {
  nsresult rv;
  const int kSeedSize = 16;
  static Atomic<uint8_t*> sSecretMidpointSeed;

  if (MOZ_UNLIKELY(!aMidpointOut)) {
    return NS_ERROR_INVALID_ARG;
  }

  /*
   * Below, we will use three different values to seed a fairly simple random
   * number generator. On the first run we initiate the secret seed, which
   * is mixed in with the time epoch and the context mix in to seed the RNG.
   *
   * This isn't the most secure method of generating a random midpoint but is
   * reasonably performant and should be sufficient for our purposes.
   */

  // If we don't have a seed, we need to get one.
  if (MOZ_UNLIKELY(!sSecretMidpointSeed)) {
    nsCOMPtr<nsIRandomGenerator> randomGenerator =
        do_GetService("@mozilla.org/security/random-generator;1", &rv);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    uint8_t* temp = nullptr;
    rv = randomGenerator->GenerateRandomBytes(kSeedSize, &temp);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    if (MOZ_UNLIKELY(!sSecretMidpointSeed.compareExchange(nullptr, temp))) {
      // Some other thread initted this first, never mind!
      delete[] temp;
    }
  }

  // sSecretMidpointSeed is now set, and invariant. The contents of the buffer
  // it points to is also invariant, _unless_ this function is called with a
  // non-null |aSecretSeed|.
  uint8_t* seed = sSecretMidpointSeed;
  MOZ_RELEASE_ASSERT(seed);

  // If someone has passed in the testing-only parameter, replace our seed with
  // it. We do _not_ re-allocate the buffer, since that can lead to UAF below.
  // The math could still be racy if the caller supplies a new secret seed while
  // some other thread is calling this function, but since this is arcane
  // test-only functionality that is used in only one test-case presently, we
  // put the burden of using this particular footgun properly on the test code.
  if (MOZ_UNLIKELY(aSecretSeed != nullptr)) {
    memcpy(seed, aSecretSeed, kSeedSize);
  }

  // Seed and create our random number generator.
  non_crypto::XorShift128PlusRNG rng(aContextMixin ^ *(uint64_t*)(seed),
                                     aClampedTimeUSec ^ *(uint64_t*)(seed + 8));

  // Retrieve the output midpoint value.
  if (MOZ_UNLIKELY(aResolutionUSec <= 0)) {  // ??? Bug 1718066
    return NS_ERROR_FAILURE;
  }
  *aMidpointOut = rng.next() % aResolutionUSec;

  return NS_OK;
}

/**
 * Given a precision value, this function will reduce a given input time to the
 * nearest multiple of that precision.
 *
 * It will check if it is appropriate to clamp the input time according to the
 * values of the given TimerPrecisionType.  Note that if one desires a minimum
 * precision for Resist Fingerprinting, it is the caller's responsibility to
 * provide the correct value. This means you should pass TimerResolution(),
 * which enforces a minimum value on the precision based on preferences.
 *
 * It ensures the given precision value is greater than zero, if it is not it
 * returns the input time.
 *
 * While the correct thing to pass is TimerResolution() we expose it as an
 * argument for testing purposes only.
 *
 * @param aTime           [in] The input time to be clamped.
 * @param aTimeScale      [in] The units the input time is in (Seconds,
 *                             Milliseconds, or Microseconds).
 * @param aResolutionUSec [in] The precision (in microseconds) to clamp to.
 * @param aContextMixin   [in] An opaque random value for relative timestamps.
 *                             0 for absolute timestamps
 * @return                 If clamping is appropriate, the clamped value of the
 *                         input, otherwise the input.
 */
/* static */
double nsRFPService::ReduceTimePrecisionImpl(double aTime, TimeScale aTimeScale,
                                             double aResolutionUSec,
                                             int64_t aContextMixin,
                                             TimerPrecisionType aType) {
  if (aType == TimerPrecisionType::DangerouslyNone) {
    return aTime;
  }

  // This boolean will serve as a flag indicating we are clamping the time
  // unconditionally. We do this when timer reduction preference is off; but we
  // still want to apply 20us clamping to al timestamps to avoid leaking
  // nano-second precision.
  bool unconditionalClamping = false;
  if (aType == UnconditionalAKAHighRes || aResolutionUSec <= 0) {
    unconditionalClamping = true;
    aResolutionUSec = RFP_TIMER_UNCONDITIONAL_VALUE;  // 20 microseconds
    aContextMixin = 0;  // Just clarifies our logging statement at the end,
                        // otherwise unused
  }

  // Increase the time as needed until it is in microseconds.
  // Note that a double can hold up to 2**53 with integer precision. This gives
  // us only until June 5, 2255 in time-since-the-epoch with integer precision.
  // So we will be losing microseconds precision after that date.
  // We think this is okay, and we codify it in some tests.
  double timeScaled = aTime * (1000000 / aTimeScale);
  // Cut off anything less than a microsecond.
  long long timeAsInt = timeScaled;

  // If we have a blank context mixin, this indicates we (should) have an
  // absolute timestamp. We check the time, and if it less than a unix timestamp
  // about 10 years in the past, we output to the log and, in debug builds,
  // assert. This is an error case we want to understand and fix: we must have
  // given a relative timestamp with a mixin of 0 which is incorrect. Anyone
  // running a debug build _probably_ has an accurate clock, and if they don't,
  // they'll hopefully find this message and understand why things are crashing.
  const long long kFeb282008 = 1204233985000;
  if (aContextMixin == 0 && timeAsInt < kFeb282008 && !unconditionalClamping &&
      aType != TimerPrecisionType::RFP) {
    nsAutoCString type;
    TypeToText(aType, type);
    MOZ_LOG(
        gResistFingerprintingLog, LogLevel::Error,
        ("About to assert. aTime=%lli<%lli aContextMixin=%" PRId64 " aType=%s",
         timeAsInt, kFeb282008, aContextMixin, type.get()));
    MOZ_ASSERT(
        false,
        "ReduceTimePrecisionImpl was given a relative time "
        "with an empty context mix-in (or your clock is 10+ years off.) "
        "Run this with MOZ_LOG=nsResistFingerprinting:1 to get more details.");
  }

  // Cast the resolution (in microseconds) to an int.
  long long resolutionAsInt = aResolutionUSec;
  // Perform the clamping.
  // We do a cast back to double to perform the division with doubles, then
  // floor the result and the rest occurs with integer precision. This is
  // because it gives consistency above and below zero. Above zero, performing
  // the division in integers truncates decimals, taking the result closer to
  // zero (a floor). Below zero, performing the division in integers truncates
  // decimals, taking the result closer to zero (a ceil). The impact of this is
  // that comparing two clamped values that should be related by a constant
  // (e.g. 10s) that are across the zero barrier will no longer work. We need to
  // round consistently towards positive infinity or negative infinity (we chose
  // negative.) This can't be done with a truncation, it must be done with
  // floor.
  long long clamped =
      floor(double(timeAsInt) / resolutionAsInt) * resolutionAsInt;

  long long midpoint = 0;
  long long clampedAndJittered = clamped;
  if (!unconditionalClamping &&
      StaticPrefs::privacy_resistFingerprinting_reduceTimerPrecision_jitter()) {
    if (!NS_FAILED(RandomMidpoint(clamped, resolutionAsInt, aContextMixin,
                                  &midpoint)) &&
        timeAsInt >= clamped + midpoint) {
      clampedAndJittered += resolutionAsInt;
    }
  }

  // Cast it back to a double and reduce it to the correct units.
  double ret = double(clampedAndJittered) / (1000000.0 / double(aTimeScale));

  MOZ_LOG(
      gResistFingerprintingLog, LogLevel::Verbose,
      ("Given: (%.*f, Scaled: %.*f, Converted: %lli), Rounding %s with (%lli, "
       "Originally %.*f), "
       "Intermediate: (%lli), Clamped: (%lli) Jitter: (%i Context: %" PRId64
       " Midpoint: %lli) "
       "Final: (%lli Converted: %.*f)",
       DBL_DIG - 1, aTime, DBL_DIG - 1, timeScaled, timeAsInt,
       (unconditionalClamping ? "unconditionally" : "normally"),
       resolutionAsInt, DBL_DIG - 1, aResolutionUSec,
       (long long)floor(double(timeAsInt) / resolutionAsInt), clamped,
       StaticPrefs::privacy_resistFingerprinting_reduceTimerPrecision_jitter(),
       aContextMixin, midpoint, clampedAndJittered, DBL_DIG - 1, ret));

  return ret;
}

/* static */
double nsRFPService::ReduceTimePrecisionAsUSecs(double aTime,
                                                int64_t aContextMixin,
                                                RTPCallerType aRTPCallerType) {
  const auto type = GetTimerPrecisionType(aRTPCallerType);
  return nsRFPService::ReduceTimePrecisionImpl(aTime, MicroSeconds,
                                               TimerResolution(aRTPCallerType),
                                               aContextMixin, type);
}

/* static */
double nsRFPService::ReduceTimePrecisionAsMSecs(double aTime,
                                                int64_t aContextMixin,
                                                RTPCallerType aRTPCallerType) {
  const auto type = GetTimerPrecisionType(aRTPCallerType);
  return nsRFPService::ReduceTimePrecisionImpl(aTime, MilliSeconds,
                                               TimerResolution(aRTPCallerType),
                                               aContextMixin, type);
}

/* static */
double nsRFPService::ReduceTimePrecisionAsMSecsRFPOnly(
    double aTime, int64_t aContextMixin, RTPCallerType aRTPCallerType) {
  return nsRFPService::ReduceTimePrecisionImpl(
      aTime, MilliSeconds, TimerResolution(aRTPCallerType), aContextMixin,
      GetTimerPrecisionTypeRFPOnly(aRTPCallerType));
}

/* static */
double nsRFPService::ReduceTimePrecisionAsSecs(double aTime,
                                               int64_t aContextMixin,
                                               RTPCallerType aRTPCallerType) {
  const auto type = GetTimerPrecisionType(aRTPCallerType);
  return nsRFPService::ReduceTimePrecisionImpl(
      aTime, Seconds, TimerResolution(aRTPCallerType), aContextMixin, type);
}

/* static */
double nsRFPService::ReduceTimePrecisionAsSecsRFPOnly(
    double aTime, int64_t aContextMixin, RTPCallerType aRTPCallerType) {
  return nsRFPService::ReduceTimePrecisionImpl(
      aTime, Seconds, TimerResolution(aRTPCallerType), aContextMixin,
      GetTimerPrecisionTypeRFPOnly(aRTPCallerType));
}

/* static */
double nsRFPService::ReduceTimePrecisionAsUSecsWrapper(double aTime,
                                                       JSContext* aCx) {
  MOZ_ASSERT(aCx);

  nsCOMPtr<nsIGlobalObject> global = xpc::CurrentNativeGlobal(aCx);
  MOZ_ASSERT(global);
  RTPCallerType callerType = global->GetRTPCallerType();
  return nsRFPService::ReduceTimePrecisionImpl(
      aTime, MicroSeconds, TimerResolution(callerType),
      0, /* For absolute timestamps (all the JS engine does), supply zero
            context mixin */
      GetTimerPrecisionType(callerType));
}

/* static */
TimerPrecisionType nsRFPService::GetTimerPrecisionType(
    RTPCallerType aRTPCallerType) {
  if (aRTPCallerType == RTPCallerType::SystemPrincipal) {
    return DangerouslyNone;
  }

  if (aRTPCallerType == RTPCallerType::ResistFingerprinting) {
    return RFP;
  }

  if (StaticPrefs::privacy_reduceTimerPrecision() &&
      aRTPCallerType == RTPCallerType::CrossOriginIsolated) {
    return UnconditionalAKAHighRes;
  }

  if (StaticPrefs::privacy_reduceTimerPrecision()) {
    return Normal;
  }

  if (StaticPrefs::privacy_reduceTimerPrecision_unconditional()) {
    return UnconditionalAKAHighRes;
  }

  return DangerouslyNone;
}

/* static */
TimerPrecisionType nsRFPService::GetTimerPrecisionTypeRFPOnly(
    RTPCallerType aRTPCallerType) {
  if (aRTPCallerType == RTPCallerType::ResistFingerprinting) {
    return RFP;
  }

  if (StaticPrefs::privacy_reduceTimerPrecision_unconditional() &&
      aRTPCallerType != RTPCallerType::SystemPrincipal) {
    return UnconditionalAKAHighRes;
  }

  return DangerouslyNone;
}

/* static */
void nsRFPService::TypeToText(TimerPrecisionType aType, nsACString& aText) {
  switch (aType) {
    case TimerPrecisionType::DangerouslyNone:
      aText.AssignLiteral("DangerouslyNone");
      return;
    case TimerPrecisionType::Normal:
      aText.AssignLiteral("Normal");
      return;
    case TimerPrecisionType::RFP:
      aText.AssignLiteral("RFP");
      return;
    case TimerPrecisionType::UnconditionalAKAHighRes:
      aText.AssignLiteral("UnconditionalAKAHighRes");
      return;
    default:
      MOZ_ASSERT(false, "Shouldn't go here");
      aText.AssignLiteral("Unknown Enum Value");
      return;
  }
}

// ============================================================================
// ============================================================================
// ============================================================================
// Video Statistics Spoofing

/* static */
uint32_t nsRFPService::CalculateTargetVideoResolution(uint32_t aVideoQuality) {
  return aVideoQuality * NSToIntCeil(aVideoQuality * 16 / 9.0);
}

/* static */
uint32_t nsRFPService::GetSpoofedTotalFrames(double aTime) {
  double precision =
      TimerResolution(RTPCallerType::ResistFingerprinting) / 1000 / 1000;
  double time = floor(aTime / precision) * precision;

  return NSToIntFloor(time * kVideoFramesPerSec);
}

/* static */
uint32_t nsRFPService::GetSpoofedDroppedFrames(double aTime, uint32_t aWidth,
                                               uint32_t aHeight) {
  uint32_t targetRes = CalculateTargetVideoResolution(
      StaticPrefs::privacy_resistFingerprinting_target_video_res());

  // The video resolution is less than or equal to the target resolution, we
  // report a zero dropped rate for this case.
  if (targetRes >= aWidth * aHeight) {
    return 0;
  }

  double precision =
      TimerResolution(RTPCallerType::ResistFingerprinting) / 1000 / 1000;
  double time = floor(aTime / precision) * precision;
  // Bound the dropped ratio from 0 to 100.
  uint32_t boundedDroppedRatio = std::min(kVideoDroppedRatio, 100U);

  return NSToIntFloor(time * kVideoFramesPerSec *
                      (boundedDroppedRatio / 100.0));
}

/* static */
uint32_t nsRFPService::GetSpoofedPresentedFrames(double aTime, uint32_t aWidth,
                                                 uint32_t aHeight) {
  uint32_t targetRes = CalculateTargetVideoResolution(
      StaticPrefs::privacy_resistFingerprinting_target_video_res());

  // The target resolution is greater than the current resolution. For this
  // case, there will be no dropped frames, so we report total frames directly.
  if (targetRes >= aWidth * aHeight) {
    return GetSpoofedTotalFrames(aTime);
  }

  double precision =
      TimerResolution(RTPCallerType::ResistFingerprinting) / 1000 / 1000;
  double time = floor(aTime / precision) * precision;
  // Bound the dropped ratio from 0 to 100.
  uint32_t boundedDroppedRatio = std::min(kVideoDroppedRatio, 100U);

  return NSToIntFloor(time * kVideoFramesPerSec *
                      ((100 - boundedDroppedRatio) / 100.0));
}

// ============================================================================
// ============================================================================
// ============================================================================
// User-Agent/Version Stuff

static const char* GetSpoofedVersion() {
#ifdef ANDROID
  // Return Desktop's ESR version.
  // When Android RFP returns an ESR version >= 120, we can remove the "rv:109"
  // spoofing in GetSpoofedUserAgent() below and stop #including
  // StaticPrefs_network.h.
  return "115.0";
#else
  return MOZILLA_UAVERSION;
#endif
}

/* static */
void nsRFPService::GetSpoofedUserAgent(nsACString& userAgent,
                                       bool isForHTTPHeader) {
  // This function generates the spoofed value of User Agent.
  // We spoof the values of the platform and Firefox version, which could be
  // used as fingerprinting sources to identify individuals.
  // Reference of the format of User Agent:
  // https://developer.mozilla.org/en-US/docs/Web/API/NavigatorID/userAgent
  // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/User-Agent

  // These magic numbers are the lengths of the UA string literals below.
  // Assume three-digit Firefox version numbers so we have room to grow.
  size_t preallocatedLength =
      13 +
      (isForHTTPHeader ? mozilla::ArrayLength(SPOOFED_HTTP_UA_OS)
                       : mozilla::ArrayLength(SPOOFED_UA_OS)) -
      1 + 5 + 3 + 10 + mozilla::ArrayLength(LEGACY_UA_GECKO_TRAIL) - 1 + 9 + 3 +
      2;
  userAgent.SetCapacity(preallocatedLength);

  const char* spoofedVersion = GetSpoofedVersion();

  // "Mozilla/5.0 (%s; rv:%d.0) Gecko/%d Firefox/%d.0"
  userAgent.AssignLiteral("Mozilla/5.0 (");

  if (isForHTTPHeader) {
    userAgent.AppendLiteral(SPOOFED_HTTP_UA_OS);
  } else {
    userAgent.AppendLiteral(SPOOFED_UA_OS);
  }

  userAgent.AppendLiteral("; rv:");

  // Desktop Firefox (regular and RFP) won't need to spoof "rv:109" in versions
  // >= 120 (bug 1806690), but Android RFP will need to continue spoofing 109
  // as long as Android's GetSpoofedVersion() returns a version < 120 above.
  uint32_t forceRV = mozilla::StaticPrefs::network_http_useragent_forceRVOnly();
  if (forceRV) {
    userAgent.Append(nsPrintfCString("%u.0", forceRV));
  } else {
    userAgent.Append(spoofedVersion);
  }

  userAgent.AppendLiteral(") Gecko/");

#if defined(ANDROID)
  userAgent.Append(spoofedVersion);
#else
  userAgent.AppendLiteral(LEGACY_UA_GECKO_TRAIL);
#endif

  userAgent.AppendLiteral(" Firefox/");
  userAgent.Append(spoofedVersion);

  MOZ_ASSERT(userAgent.Length() <= preallocatedLength);
}

// ============================================================================
// ============================================================================
// ============================================================================
// Keyboard Spoofing Stuff

nsTHashMap<KeyboardHashKey, const SpoofingKeyboardCode*>*
    nsRFPService::sSpoofingKeyboardCodes = nullptr;

KeyboardHashKey::KeyboardHashKey(const KeyboardLangs aLang,
                                 const KeyboardRegions aRegion,
                                 const KeyNameIndexType aKeyIdx,
                                 const nsAString& aKey)
    : mLang(aLang), mRegion(aRegion), mKeyIdx(aKeyIdx), mKey(aKey) {}

KeyboardHashKey::KeyboardHashKey(KeyTypePointer aOther)
    : mLang(aOther->mLang),
      mRegion(aOther->mRegion),
      mKeyIdx(aOther->mKeyIdx),
      mKey(aOther->mKey) {}

KeyboardHashKey::KeyboardHashKey(KeyboardHashKey&& aOther) noexcept
    : PLDHashEntryHdr(std::move(aOther)),
      mLang(std::move(aOther.mLang)),
      mRegion(std::move(aOther.mRegion)),
      mKeyIdx(std::move(aOther.mKeyIdx)),
      mKey(std::move(aOther.mKey)) {}

KeyboardHashKey::~KeyboardHashKey() = default;

bool KeyboardHashKey::KeyEquals(KeyTypePointer aOther) const {
  return mLang == aOther->mLang && mRegion == aOther->mRegion &&
         mKeyIdx == aOther->mKeyIdx && mKey == aOther->mKey;
}

KeyboardHashKey::KeyTypePointer KeyboardHashKey::KeyToPointer(KeyType aKey) {
  return &aKey;
}

PLDHashNumber KeyboardHashKey::HashKey(KeyTypePointer aKey) {
  PLDHashNumber hash = mozilla::HashString(aKey->mKey);
  return mozilla::AddToHash(hash, aKey->mRegion, aKey->mKeyIdx, aKey->mLang);
}

/* static */
void nsRFPService::MaybeCreateSpoofingKeyCodes(const KeyboardLangs aLang,
                                               const KeyboardRegions aRegion) {
  if (sSpoofingKeyboardCodes == nullptr) {
    sSpoofingKeyboardCodes =
        new nsTHashMap<KeyboardHashKey, const SpoofingKeyboardCode*>();
  }

  if (KeyboardLang::EN == aLang) {
    switch (aRegion) {
      case KeyboardRegion::US:
        MaybeCreateSpoofingKeyCodesForEnUS();
        break;
    }
  }
}

/* static */
void nsRFPService::MaybeCreateSpoofingKeyCodesForEnUS() {
  MOZ_ASSERT(sSpoofingKeyboardCodes);

  static bool sInitialized = false;
  const KeyboardLangs lang = KeyboardLang::EN;
  const KeyboardRegions reg = KeyboardRegion::US;

  if (sInitialized) {
    return;
  }

  static const SpoofingKeyboardInfo spoofingKeyboardInfoTable[] = {
#define KEY(key_, _codeNameIdx, _keyCode, _modifier) \
  {NS_LITERAL_STRING_FROM_CSTRING(key_),             \
   KEY_NAME_INDEX_USE_STRING,                        \
   {CODE_NAME_INDEX_##_codeNameIdx, _keyCode, _modifier}},
#define CONTROL(keyNameIdx_, _codeNameIdx, _keyCode) \
  {u""_ns,                                           \
   KEY_NAME_INDEX_##keyNameIdx_,                     \
   {CODE_NAME_INDEX_##_codeNameIdx, _keyCode, MODIFIER_NONE}},
#include "KeyCodeConsensus_En_US.h"
#undef CONTROL
#undef KEY
  };

  for (const auto& keyboardInfo : spoofingKeyboardInfoTable) {
    KeyboardHashKey key(lang, reg, keyboardInfo.mKeyIdx, keyboardInfo.mKey);
    MOZ_ASSERT(!sSpoofingKeyboardCodes->Contains(key),
               "Double-defining key code; fix your KeyCodeConsensus file");
    sSpoofingKeyboardCodes->InsertOrUpdate(key, &keyboardInfo.mSpoofingCode);
  }

  sInitialized = true;
}

/* static */
void nsRFPService::GetKeyboardLangAndRegion(const nsAString& aLanguage,
                                            KeyboardLangs& aLocale,
                                            KeyboardRegions& aRegion) {
  nsAutoString langStr;
  nsAutoString regionStr;
  uint32_t partNum = 0;

  for (const nsAString& part : aLanguage.Split('-')) {
    if (partNum == 0) {
      langStr = part;
    } else {
      regionStr = part;
      break;
    }

    partNum++;
  }

  // We test each language here as well as the region. There are some cases that
  // only the language is given, we will use the default region code when this
  // happens. The default region should depend on the given language.
  if (langStr.EqualsLiteral(RFP_KEYBOARD_LANG_STRING_EN)) {
    aLocale = KeyboardLang::EN;
    // Give default values first.
    aRegion = KeyboardRegion::US;

    if (regionStr.EqualsLiteral(RFP_KEYBOARD_REGION_STRING_US)) {
      aRegion = KeyboardRegion::US;
    }
  } else {
    // There is no spoofed keyboard locale for the given language. We use the
    // default one in this case.
    aLocale = RFP_DEFAULT_SPOOFING_KEYBOARD_LANG;
    aRegion = RFP_DEFAULT_SPOOFING_KEYBOARD_REGION;
  }
}

/* static */
bool nsRFPService::GetSpoofedKeyCodeInfo(
    const dom::Document* aDoc, const WidgetKeyboardEvent* aKeyboardEvent,
    SpoofingKeyboardCode& aOut) {
  MOZ_ASSERT(aKeyboardEvent);

  KeyboardLangs keyboardLang = RFP_DEFAULT_SPOOFING_KEYBOARD_LANG;
  KeyboardRegions keyboardRegion = RFP_DEFAULT_SPOOFING_KEYBOARD_REGION;
  // If the document is given, we use the content language which is get from the
  // document. Otherwise, we use the default one.
  if (aDoc != nullptr) {
    nsAutoString language;
    aDoc->GetContentLanguage(language);

    // If the content-langauge is not given, we try to get langauge from the
    // HTML lang attribute.
    if (language.IsEmpty()) {
      dom::Element* elm = aDoc->GetHtmlElement();

      if (elm != nullptr) {
        elm->GetLang(language);
      }
    }

    // If two or more languages are given, per HTML5 spec, we should consider
    // it as 'unknown'. So we use the default one.
    if (!language.IsEmpty() && !language.Contains(char16_t(','))) {
      language.StripWhitespace();
      GetKeyboardLangAndRegion(language, keyboardLang, keyboardRegion);
    }
  }

  MaybeCreateSpoofingKeyCodes(keyboardLang, keyboardRegion);

  KeyNameIndex keyIdx = aKeyboardEvent->mKeyNameIndex;
  nsAutoString keyName;

  if (keyIdx == KEY_NAME_INDEX_USE_STRING) {
    keyName = aKeyboardEvent->mKeyValue;
  }

  KeyboardHashKey key(keyboardLang, keyboardRegion, keyIdx, keyName);
  const SpoofingKeyboardCode* keyboardCode = sSpoofingKeyboardCodes->Get(key);

  if (keyboardCode != nullptr) {
    aOut = *keyboardCode;
    return true;
  }

  return false;
}

/* static */
bool nsRFPService::GetSpoofedModifierStates(
    const dom::Document* aDoc, const WidgetKeyboardEvent* aKeyboardEvent,
    const Modifiers aModifier, bool& aOut) {
  MOZ_ASSERT(aKeyboardEvent);

  // For modifier or control keys, we don't need to hide its modifier states.
  if (aKeyboardEvent->mKeyNameIndex != KEY_NAME_INDEX_USE_STRING) {
    return false;
  }

  // We will spoof the modifer state for Alt, Shift, and AltGraph.
  // We don't spoof the Control key, because it is often used
  // for command key combinations in web apps.
  if ((aModifier & (MODIFIER_ALT | MODIFIER_SHIFT | MODIFIER_ALTGRAPH)) != 0) {
    SpoofingKeyboardCode keyCodeInfo;

    if (GetSpoofedKeyCodeInfo(aDoc, aKeyboardEvent, keyCodeInfo)) {
      aOut = ((keyCodeInfo.mModifierStates & aModifier) != 0);
      return true;
    }
  }

  return false;
}

/* static */
bool nsRFPService::GetSpoofedCode(const dom::Document* aDoc,
                                  const WidgetKeyboardEvent* aKeyboardEvent,
                                  nsAString& aOut) {
  MOZ_ASSERT(aKeyboardEvent);

  SpoofingKeyboardCode keyCodeInfo;

  if (!GetSpoofedKeyCodeInfo(aDoc, aKeyboardEvent, keyCodeInfo)) {
    return false;
  }

  WidgetKeyboardEvent::GetDOMCodeName(keyCodeInfo.mCode, aOut);

  // We need to change the 'Left' with 'Right' if the location indicates
  // it's a right key.
  if (aKeyboardEvent->mLocation ==
          dom::KeyboardEvent_Binding::DOM_KEY_LOCATION_RIGHT &&
      StringEndsWith(aOut, u"Left"_ns)) {
    aOut.ReplaceLiteral(aOut.Length() - 4, 4, u"Right");
  }

  return true;
}

/* static */
bool nsRFPService::GetSpoofedKeyCode(const dom::Document* aDoc,
                                     const WidgetKeyboardEvent* aKeyboardEvent,
                                     uint32_t& aOut) {
  MOZ_ASSERT(aKeyboardEvent);

  SpoofingKeyboardCode keyCodeInfo;

  if (GetSpoofedKeyCodeInfo(aDoc, aKeyboardEvent, keyCodeInfo)) {
    aOut = keyCodeInfo.mKeyCode;
    return true;
  }

  return false;
}

// ============================================================================
// ============================================================================
// ============================================================================
// Randomization Stuff
nsresult nsRFPService::EnsureSessionKey(bool aIsPrivate) {
  MOZ_ASSERT(XRE_IsParentProcess());

  MOZ_LOG(gResistFingerprintingLog, LogLevel::Info,
          ("Ensure the session key for %s browsing session\n",
           aIsPrivate ? "private" : "normal"));

  // If any fingerprinting randomization protection is enabled, we generate the
  // session key.
  // Note that there is only canvas randomization protection currently.
  if (!nsContentUtils::ShouldResistFingerprinting(
          "Checking the target activation globally without local context",
          RFPTarget::CanvasRandomization)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  Maybe<nsID>& sessionKey =
      aIsPrivate ? mPrivateBrowsingSessionKey : mBrowsingSessionKey;

  // The key has been generated, bail out earlier.
  if (sessionKey) {
    MOZ_LOG(
        gResistFingerprintingLog, LogLevel::Info,
        ("The %s session key exists: %s\n", aIsPrivate ? "private" : "normal",
         sessionKey.ref().ToString().get()));
    return NS_OK;
  }

  sessionKey.emplace(nsID::GenerateUUID());

  MOZ_LOG(gResistFingerprintingLog, LogLevel::Debug,
          ("Generated %s session key: %s\n", aIsPrivate ? "private" : "normal",
           sessionKey.ref().ToString().get()));

  return NS_OK;
}

void nsRFPService::ClearSessionKey(bool aIsPrivate) {
  MOZ_ASSERT(XRE_IsParentProcess());

  Maybe<nsID>& sessionKey =
      aIsPrivate ? mPrivateBrowsingSessionKey : mBrowsingSessionKey;

  sessionKey.reset();
}

// static
Maybe<nsTArray<uint8_t>> nsRFPService::GenerateKey(nsIChannel* aChannel) {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(aChannel);

#ifdef DEBUG
  // Ensure we only compute random key for top-level loads.
  {
    nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
    MOZ_ASSERT(loadInfo->GetExternalContentPolicyType() ==
               ExtContentPolicy::TYPE_DOCUMENT);
  }
#endif

  nsCOMPtr<nsIURI> topLevelURI;
  Unused << aChannel->GetURI(getter_AddRefs(topLevelURI));
  bool isPrivate = NS_UsePrivateBrowsing(aChannel);

  MOZ_LOG(gResistFingerprintingLog, LogLevel::Debug,
          ("Generating %s randomization key for top-level URI: %s\n",
           isPrivate ? "private" : "normal",
           topLevelURI->GetSpecOrDefault().get()));

  RefPtr<nsRFPService> service = GetOrCreate();

  if (NS_FAILED(service->EnsureSessionKey(isPrivate))) {
    return Nothing();
  }

  // Return nothing if fingerprinting randomization is disabled for the given
  // channel.
  //
  // Note that canvas randomization is the only fingerprinting randomization
  // protection currently.
  if (!nsContentUtils::ShouldResistFingerprinting(
          aChannel, RFPTarget::CanvasRandomization)) {
    return Nothing();
  }

  const nsID& sessionKey = isPrivate ? service->mPrivateBrowsingSessionKey.ref()
                                     : service->mBrowsingSessionKey.ref();

  auto sessionKeyStr = sessionKey.ToString();

  // Using the OriginAttributes to get the site from the top-level URI. The site
  // is composed of scheme, host, and port.
  OriginAttributes attrs;
  attrs.SetPartitionKey(topLevelURI);

  // Generate the key by using the hMAC. The key is based on the session key and
  // the partitionKey, i.e. top-level site.
  HMAC hmac;

  nsresult rv = hmac.Begin(
      SEC_OID_SHA256,
      Span(reinterpret_cast<const uint8_t*>(sessionKeyStr.get()), NSID_LENGTH));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return Nothing();
  }

  NS_ConvertUTF16toUTF8 topLevelSite(attrs.mPartitionKey);
  rv = hmac.Update(reinterpret_cast<const uint8_t*>(topLevelSite.get()),
                   topLevelSite.Length());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return Nothing();
  }

  Maybe<nsTArray<uint8_t>> key;
  key.emplace();

  rv = hmac.End(key.ref());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return Nothing();
  }

  return key;
}

// static
nsresult nsRFPService::GenerateCanvasKeyFromImageData(
    nsICookieJarSettings* aCookieJarSettings, uint8_t* aImageData,
    uint32_t aSize, nsTArray<uint8_t>& aCanvasKey) {
  NS_ENSURE_ARG_POINTER(aCookieJarSettings);

  nsTArray<uint8_t> randomKey;
  nsresult rv =
      aCookieJarSettings->GetFingerprintingRandomizationKey(randomKey);

  // There is no random key for this cookieJarSettings. This means that the
  // randomization is disabled. So, we can bail out from here without doing
  // anything.
  if (NS_FAILED(rv)) {
    return NS_ERROR_FAILURE;
  }

  // Generate the key for randomizing the canvas data using hMAC. The key is
  // based on the random key of the document and the canvas data itself. So,
  // different canvas would have different keys.
  HMAC hmac;

  rv = hmac.Begin(SEC_OID_SHA256, Span(randomKey));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = hmac.Update(aImageData, aSize);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = hmac.End(aCanvasKey);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

// static
nsresult nsRFPService::RandomizePixels(nsICookieJarSettings* aCookieJarSettings,
                                       uint8_t* aData, uint32_t aSize,
                                       gfx::SurfaceFormat aSurfaceFormat) {
  NS_ENSURE_ARG_POINTER(aData);

  if (!aCookieJarSettings) {
    return NS_OK;
  }

  if (aSize == 0) {
    return NS_OK;
  }

  auto timerId =
      glean::fingerprinting_protection::canvas_noise_calculate_time.Start();

  nsTArray<uint8_t> canvasKey;
  nsresult rv = GenerateCanvasKeyFromImageData(aCookieJarSettings, aData, aSize,
                                               canvasKey);
  if (NS_FAILED(rv)) {
    glean::fingerprinting_protection::canvas_noise_calculate_time.Cancel(
        std::move(timerId));
    return rv;
  }

  // Calculate the number of pixels based on the given data size. One pixel uses
  // 4 bytes that contains ARGB information.
  uint32_t pixelCnt = aSize / 4;

  // Generate random values that will decide the RGB channel and the pixel
  // position that we are going to introduce the noises. The channel and
  // position are predictable to ensure we have a consistant result with the
  // same canvas in the same browsing session.

  // Seed and create the first random number generator which will be used to
  // select RGB channel and the pixel position. The seed is the first half of
  // the canvas key.
  non_crypto::XorShift128PlusRNG rng1(
      *reinterpret_cast<uint64_t*>(canvasKey.Elements()),
      *reinterpret_cast<uint64_t*>(canvasKey.Elements() + 8));

  // Use the last 8 bits as the number of noises.
  uint8_t rnd3 = canvasKey.LastElement();

  // Clear the last 8 bits.
  canvasKey.ReplaceElementAt(canvasKey.Length() - 1, 0);

  // Use the remaining 120 bits to seed and create the second random number
  // generator. The random number will be used to decided the noise bit that
  // will be added to the lowest order bit of the channel of the pixel.
  non_crypto::XorShift128PlusRNG rng2(
      *reinterpret_cast<uint64_t*>(canvasKey.Elements() + 16),
      *reinterpret_cast<uint64_t*>(canvasKey.Elements() + 24));

  // Ensure at least 16 random changes may occur.
  uint8_t numNoises = std::clamp<uint8_t>(rnd3, 15, 255);

  for (uint8_t i = 0; i <= numNoises; i++) {
    // Choose which RGB channel to add a noise. The pixel data is in either
    // the BGRA or the ARGB format depending on the endianess. To choose the
    // color channel we need to add the offset according the endianess.
    uint32_t channel;
    if (aSurfaceFormat == gfx::SurfaceFormat::B8G8R8A8) {
      channel = rng1.next() % 3;
    } else if (aSurfaceFormat == gfx::SurfaceFormat::A8R8G8B8) {
      channel = rng1.next() % 3 + 1;
    } else {
      return NS_ERROR_INVALID_ARG;
    }

    uint32_t idx = 4 * (rng1.next() % pixelCnt) + channel;
    uint8_t bit = rng2.next();

    aData[idx] = aData[idx] ^ (bit & 0x1);
  }

  glean::fingerprinting_protection::canvas_noise_calculate_time
      .StopAndAccumulate(std::move(timerId));

  return NS_OK;
}
