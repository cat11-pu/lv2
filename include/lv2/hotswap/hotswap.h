// Copyright 2026 LV2 contributors
// SPDX-License-Identifier: ISC

#ifndef LV2_HOTSWAP_HOTSWAP_H
#define LV2_HOTSWAP_HOTSWAP_H

/**
   @defgroup hotswap Hot Swap
   @ingroup lv2

   Replacing a running plugin instance without interrupting playback.

   See <http://lv2plug.in/ns/ext/hotswap> for details.

   @{
*/

#include <lv2/core/lv2.h>

#include <stdint.h>

// clang-format off

#define LV2_HOTSWAP_URI    "http://lv2plug.in/ns/ext/hotswap"  ///< http://lv2plug.in/ns/ext/hotswap
#define LV2_HOTSWAP_PREFIX LV2_HOTSWAP_URI "#"                 ///< http://lv2plug.in/ns/ext/hotswap#

#define LV2_HOTSWAP__interface LV2_HOTSWAP_PREFIX "interface"  ///< http://lv2plug.in/ns/ext/hotswap#interface

// clang-format on

#ifdef __cplusplus
extern "C" {
#endif

/** Status code for hot swap operations. */
typedef enum {
  LV2_HOTSWAP_SUCCESS           = 0, /**< Completed successfully. */
  LV2_HOTSWAP_ERR_UNKNOWN       = 1, /**< Unknown error. */
  LV2_HOTSWAP_ERR_BAD_ARGUMENT  = 2, /**< Invalid argument. */
  LV2_HOTSWAP_ERR_BUSY          = 3, /**< A swap is already prepared. */
  LV2_HOTSWAP_ERR_NOT_PREPARED  = 4, /**< No swap is prepared. */
  LV2_HOTSWAP_ERR_NO_MEMORY     = 5, /**< Failed to allocate memory. */
  LV2_HOTSWAP_ERR_BAD_PLUGIN    = 6, /**< Incompatible plugin or binary. */
  LV2_HOTSWAP_ERR_BAD_PORT      = 7, /**< Invalid or incompatible port map. */
  LV2_HOTSWAP_ERR_BAD_STATE     = 8, /**< Incompatible or invalid state. */
  LV2_HOTSWAP_ERR_UNSUPPORTED   = 9, /**< Required functionality is missing. */
  LV2_HOTSWAP_ERR_NOT_BOUNDARY  = 10 /**< Not at a safe block boundary. */
} LV2_Hotswap_Status;

/** Flags for a port mapping. */
typedef enum {
  LV2_HOTSWAP_PORT_INPUT    = 1U << 0U, /**< Audio/control/event input. */
  LV2_HOTSWAP_PORT_OUTPUT   = 1U << 1U, /**< Audio/control/event output. */
  LV2_HOTSWAP_PORT_OPTIONAL = 1U << 2U /**< Port may be unconnected. */
} LV2_Hotswap_Port_Flags;

/** Flags for a swap target. */
typedef enum {
  /**
     Fail preparation unless both instances provide LV2_HOTSWAP__interface.

     Hosts that require bit-exact continuation SHOULD set this.  Without it, a
     plugin that does not provide the migration interface is replaced with a
     fresh instance whose state is reset.
  */
  LV2_HOTSWAP_F_REQUIRE_STATE = 1U << 0U
} LV2_Hotswap_Target_Flags;

/**
   A port mapping from an old instance to a new instance.

   Ports are identified by their persistent symbol, never by index.  The host
   MUST connect the new instance to the same host-owned buffer that the old
   port used.  `old_type` and `new_type` are URIDs for the primary port type
   (for example lv2:AudioPort or atom:AtomPort), or 0 if unknown.  Non-zero
   types MUST match.
*/
typedef struct {
  const char* symbol;    /**< Persistent port symbol. */
  uint32_t    old_index; /**< Port index on the old instance. */
  uint32_t    new_index; /**< Port index on the new instance. */
  uint32_t    old_type;  /**< Old port type URID, or 0. */
  uint32_t    new_type;  /**< New port type URID, or 0. */
  uint32_t    flags;     /**< LV2_Hotswap_Port_Flags. */
} LV2_Hotswap_Port;

/**
   Description of an instance to prepare.

   `descriptor` MUST have the same plugin URI as the running instance.  It may
   be a different binary or build with different port indices.  `features` MUST
   be the same host feature set used for the running instance, with
   sample-rate-dependent options adjusted for `sample_rate`.
*/
typedef struct {
  const LV2_Descriptor*     descriptor;     /**< New plugin descriptor. */
  const char*               bundle_path;    /**< New plugin bundle path. */
  const LV2_Feature* const* features;       /**< NULL-terminated features. */
  double                    sample_rate;    /**< New sample rate in Hz. */
  const LV2_Hotswap_Port*   ports;          /**< Port mapping array. */
  uint32_t                  num_ports;      /**< Number of port mappings. */
  uint32_t                  max_state_size; /**< Budget, or 0 for no limit. */
  uint32_t                  flags;          /**< LV2_Hotswap_Target_Flags. */
} LV2_Hotswap_Target;

/**
   An absolute playback position on a fixed host timeline.

   `frame` is a frame number at `rate`, the host's reference sample rate.  This
   rate SHOULD remain constant for the lifetime of a session, so positions are
   never converted cumulatively and rounding cannot drift across swaps.
*/
typedef struct {
  uint64_t frame; /**< Absolute frame number at `rate`. */
  uint32_t rate;  /**< Reference sample rate in Hz. */
} LV2_Hotswap_Position;

/**
   Plugin-provided state migration interface (LV2_HOTSWAP__interface).

   `state_size()` is in the instantiation threading class.  `save_state()` and
   `restore_state()` are in the audio threading class: they MUST be wait-free,
   MUST NOT allocate, lock, block, perform I/O, or call host callbacks, and
   MUST complete in bounded time.  The saved state MUST be usable by an
   instance with the same plugin URI running at a different sample rate.

   `state_version` identifies the binary state layout.  The old and new
   instances MUST report the same version and state size.
*/
typedef struct {
  uint64_t state_version; /**< Plugin-specific state layout version. */

  /** Return the required state size in bytes for `instance`. */
  uint32_t (*state_size)(LV2_Handle instance);

  /** Copy exactly `size` bytes of real-time-safe state to `state`. */
  LV2_Hotswap_Status (*save_state)(LV2_Handle instance,
                                   void*      state,
                                   uint32_t   size);

  /** Restore exactly `size` bytes of real-time-safe state from `state`. */
  LV2_Hotswap_Status (*restore_state)(LV2_Handle   instance,
                                      const void*  state,
                                      uint32_t     size);
} LV2_Hotswap_Plugin_Interface;

/**
   Host-provided hot swap interface.

   The host exposes this interface to its graph/engine code.  Calls to
   `prepare()` and `abort()` are in the instantiation threading class and must
   not be concurrent with each other.  `commit()` is in the audio threading
   class and MUST be called at a block boundary, when `run()` is not executing.

   `prepare()`:

   - Runs outside the audio thread and may allocate.
   - Instantiates and activates the new instance, connects its ports to the
     existing host buffers by symbol, and validates the port/state metadata.
   - Does not affect the running instance or the event queue.

   `commit()`:

   - Is wait-free with respect to allocation, locks, I/O, and plugin lifecycle
     calls.  It saves old state and restores new state using preallocated
     storage, then atomically publishes the new instance.
   - Does not reset, copy, or discard queued events.  Pending events use
     absolute positions on the fixed LV2_Hotswap_Position timeline and are
     delivered exactly once by the new instance.
   - Defers deactivation and cleanup of the old instance to a non-real-time
     reaper.

   `abort()` discards a prepared instance and leaves the running instance,
   position, and event queue untouched.  It is safe to call when nothing is
   prepared.
*/
typedef struct {
  void* handle; /**< Opaque host data. */

  /** Prepare a replacement instance without touching the running instance. */
  LV2_Hotswap_Status (*prepare)(void* handle,
                                const LV2_Hotswap_Target* target);

  /** Atomically publish a prepared instance at a block boundary. */
  LV2_Hotswap_Status (*commit)(void* handle,
                               const LV2_Hotswap_Position* position);

  /** Discard a prepared instance, if any. */
  void (*abort)(void* handle);
} LV2_Hotswap_Host_Interface;

/**
   Map a frame position from one sample rate to another.

   Returns floor(frame * to / from).  This implementation avoids overflow for
   all `uint64_t` frame values and returns UINT64_MAX on overflow.
*/
static inline uint64_t
lv2_hotswap_scale_frame(uint64_t frame, uint32_t from, uint32_t to)
{
  if (from == 0U || to == 0U) {
    return 0U;
  }

  if (from == to) {
    return frame;
  }

  const uint64_t quotient  = frame / from;
  const uint64_t remainder = frame % from;
  if (quotient > UINT64_MAX / to) {
    return UINT64_MAX;
  }

  return (quotient * to) + ((remainder * to) / from);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

/**
   @}
*/

#endif // LV2_HOTSWAP_HOTSWAP_H
