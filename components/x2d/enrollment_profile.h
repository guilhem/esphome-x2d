#pragma once

#include <x2d/controller.h>

// The lifecycle core owns slot selection and durable attempt bounds. Refuse old
// private flags rather than silently presenting them as admission restrictions.
#if defined(X2D_TRIAL_SLOT) || defined(X2D_TRIAL_EXPECTED_NEXT_COUNTER)
#error "X2D_TRIAL_SLOT and X2D_TRIAL_EXPECTED_NEXT_COUNTER are obsolete; remove them. The journal selects slots and limits explicit retries."
#endif

#ifdef X2D_TRIAL_IDENTITY_SUFFIX
static_assert(X2D_TRIAL_IDENTITY_SUFFIX >= 0 && X2D_TRIAL_IDENTITY_SUFFIX <= 255,
              "Trial suffix must be a byte");
#elif defined(X2D_ENROLLMENT_ENABLED)
#error "Enrollment requires a private X2D_TRIAL_IDENTITY_SUFFIX build flag"
#endif

namespace esphome::x2d {
inline constexpr ::x2d::EnrollmentProfile enrollment_profile() {
#ifdef X2D_TRIAL_IDENTITY_SUFFIX
  return {X2D_TRIAL_IDENTITY_SUFFIX};
#else
  return {};
#endif
}
}  // namespace esphome::x2d
