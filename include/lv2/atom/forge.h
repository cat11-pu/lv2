// Copyright 2008-2016 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

/**
   @file forge.h An API for constructing LV2 atoms.

   This file provides an API for constructing Atoms which makes it relatively
   simple to build nested atoms of arbitrary complexity without requiring
   dynamic memory allocation.

   The API is based on successively appending the appropriate pieces to build a
   complete Atom.  The size of containers is automatically updated.  Functions
   that begin a container return (via their frame argument) a stack frame which
   must be popped when the container is finished.

   All output is written to a user-provided buffer or sink function.  This
   makes it possible to create atoms on the stack, on the heap, in LV2 port
   buffers, in a ringbuffer, or elsewhere, all using the same API.

   This entire API is realtime safe if used with a buffer or a realtime safe
   sink, except lv2_atom_forge_init() which is only realtime safe if the URI
   map function is.

   Note these functions are all static inline, do not take their address.

   This header is non-normative, it is provided for convenience.
*/

#ifndef LV2_ATOM_FORGE_H
#define LV2_ATOM_FORGE_H

/**
   @defgroup forge Forge
   @ingroup atom

   An API for constructing LV2 atoms.

   @{
*/

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/attributes.h>
#include <lv2/urid/urid.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Disable deprecation warnings for Blank and Resource
LV2_DISABLE_DEPRECATION_WARNINGS

/** Handle for LV2_Atom_Forge_Sink. */
typedef void* LV2_Atom_Forge_Sink_Handle;

/** A reference to a chunk of written output. */
typedef intptr_t LV2_Atom_Forge_Ref;

/** Result of a forge operation. */
typedef enum {
  LV2_ATOM_FORGE_SUCCESS = 0,             /**< Operation succeeded. */
  LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT,    /**< A required pointer was null. */
  LV2_ATOM_FORGE_ERR_INVALID_INDEX,       /**< Buffer index out of range. */
  LV2_ATOM_FORGE_ERR_INVALID_RESOURCE,    /**< A required URID was zero. */
  LV2_ATOM_FORGE_ERR_INVALID_LIFECYCLE,   /**< Frames were not LIFO. */
  LV2_ATOM_FORGE_ERR_NO_SPACE,            /**< The output buffer is full. */
  LV2_ATOM_FORGE_ERR_OVERFLOW             /**< A size calculation overflowed. */
} LV2_Atom_Forge_Status;

/** Sink function for writing output.  See lv2_atom_forge_set_sink(). */
typedef LV2_Atom_Forge_Ref (*LV2_Atom_Forge_Sink)(
  LV2_Atom_Forge_Sink_Handle handle,
  const void*                buf,
  uint32_t                   size);

/** Function for resolving a reference.  See lv2_atom_forge_set_sink(). */
typedef LV2_Atom* (*LV2_Atom_Forge_Deref_Func)(
  LV2_Atom_Forge_Sink_Handle handle,
  LV2_Atom_Forge_Ref         ref);

/** A stack frame used for keeping track of nested Atom containers. */
typedef struct LV2_Atom_Forge_Frame {
  struct LV2_Atom_Forge_Frame* parent;
  LV2_Atom_Forge_Ref           ref;
} LV2_Atom_Forge_Frame;

/** A "forge" for creating atoms by appending to a buffer. */
typedef struct {
  uint8_t* buf;
  uint32_t offset;
  uint32_t size;

  LV2_Atom_Forge_Sink        sink;
  LV2_Atom_Forge_Deref_Func  deref;
  LV2_Atom_Forge_Sink_Handle handle;

  LV2_Atom_Forge_Frame* stack;

  LV2_URID Blank LV2_DEPRECATED;
  LV2_URID Bool;
  LV2_URID Chunk;
  LV2_URID Double;
  LV2_URID Float;
  LV2_URID Int;
  LV2_URID Long;
  LV2_URID Literal;
  LV2_URID Object;
  LV2_URID Path;
  LV2_URID Property;
  LV2_URID Resource LV2_DEPRECATED;
  LV2_URID Sequence;
  LV2_URID String;
  LV2_URID Tuple;
  LV2_URID URI;
  LV2_URID URID;
  LV2_URID Vector;

  LV2_Atom_Forge_Status status;
} LV2_Atom_Forge;

static inline void
lv2_atom_forge_set_buffer(LV2_Atom_Forge* forge, uint8_t* buf, size_t size);

/** Return a human-readable description of a forge status. */
static inline const char*
lv2_atom_forge_status_string(LV2_Atom_Forge_Status status)
{
  switch (status) {
  case LV2_ATOM_FORGE_SUCCESS:
    return "success";
  case LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT:
    return "invalid argument";
  case LV2_ATOM_FORGE_ERR_INVALID_INDEX:
    return "invalid buffer index";
  case LV2_ATOM_FORGE_ERR_INVALID_RESOURCE:
    return "missing resource identifier";
  case LV2_ATOM_FORGE_ERR_INVALID_LIFECYCLE:
    return "invalid container lifecycle";
  case LV2_ATOM_FORGE_ERR_NO_SPACE:
    return "insufficient space";
  case LV2_ATOM_FORGE_ERR_OVERFLOW:
    return "size overflow";
  }

  return "unknown error";
}

/** Return the status of the last forge operation. */
static inline LV2_Atom_Forge_Status
lv2_atom_forge_get_status(const LV2_Atom_Forge* forge)
{
  return forge ? forge->status : LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
}

/** Return true iff `forge` is valid and has no pending error. */
static inline bool
lv2_atom_forge_ok(const LV2_Atom_Forge* forge)
{
  return forge && forge->status == LV2_ATOM_FORGE_SUCCESS;
}

/**
   Initialise `forge`.

   URIs will be mapped using `map` and stored, a reference to `map` itself is
   not held.
*/
static inline void
lv2_atom_forge_init(LV2_Atom_Forge* forge, LV2_URID_Map* map)
{
  if (!forge) {
    return;
  }

  memset(forge, 0, sizeof(*forge));
  lv2_atom_forge_set_buffer(forge, NULL, 0);

  if (!map || !map->map) {
    forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    return;
  }

  forge->Blank    = map->map(map->handle, LV2_ATOM__Blank);
  forge->Bool     = map->map(map->handle, LV2_ATOM__Bool);
  forge->Chunk    = map->map(map->handle, LV2_ATOM__Chunk);
  forge->Double   = map->map(map->handle, LV2_ATOM__Double);
  forge->Float    = map->map(map->handle, LV2_ATOM__Float);
  forge->Int      = map->map(map->handle, LV2_ATOM__Int);
  forge->Long     = map->map(map->handle, LV2_ATOM__Long);
  forge->Literal  = map->map(map->handle, LV2_ATOM__Literal);
  forge->Object   = map->map(map->handle, LV2_ATOM__Object);
  forge->Path     = map->map(map->handle, LV2_ATOM__Path);
  forge->Property = map->map(map->handle, LV2_ATOM__Property);
  forge->Resource = map->map(map->handle, LV2_ATOM__Resource);
  forge->Sequence = map->map(map->handle, LV2_ATOM__Sequence);
  forge->String   = map->map(map->handle, LV2_ATOM__String);
  forge->Tuple    = map->map(map->handle, LV2_ATOM__Tuple);
  forge->URI      = map->map(map->handle, LV2_ATOM__URI);
  forge->URID     = map->map(map->handle, LV2_ATOM__URID);
  forge->Vector   = map->map(map->handle, LV2_ATOM__Vector);

  if (!forge->Blank || !forge->Bool || !forge->Chunk || !forge->Double ||
      !forge->Float || !forge->Int || !forge->Long || !forge->Literal ||
      !forge->Object || !forge->Path || !forge->Property || !forge->Resource ||
      !forge->Sequence || !forge->String || !forge->Tuple || !forge->URI ||
      !forge->URID || !forge->Vector) {
    forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
  }
}

/** Access the Atom pointed to by a reference. */
static inline LV2_Atom*
lv2_atom_forge_deref(LV2_Atom_Forge* forge, LV2_Atom_Forge_Ref ref)
{
  if (!forge || !ref) {
    return NULL;
  }

  if (forge->buf) {
    const uintptr_t begin = (uintptr_t)forge->buf;
    const uintptr_t addr  = (uintptr_t)ref;
    if (forge->size < sizeof(LV2_Atom) || addr < begin ||
        addr - begin > forge->size - sizeof(LV2_Atom)) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_INDEX;
      return NULL;
    }

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return (LV2_Atom*)ref;
  }

  if (!forge->deref) {
    forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    return NULL;
  }

  LV2_Atom* atom = forge->deref(forge->handle, ref);
  if (!atom) {
    forge->status = LV2_ATOM_FORGE_ERR_INVALID_INDEX;
  }

  return atom;
}

/**
   @name Object Stack
   @{
*/

/**
   Push a stack frame.
   This is done automatically by container functions (which take a stack frame
   pointer), but may be called by the user to push the top level container when
   writing to an existing Atom.
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_push(LV2_Atom_Forge*       forge,
                    LV2_Atom_Forge_Frame* frame,
                    LV2_Atom_Forge_Ref    ref)
{
  if (!forge || !frame) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    }
    return 0;
  }

  for (const LV2_Atom_Forge_Frame* f = forge->stack; f; f = f->parent) {
    if (f == frame) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_LIFECYCLE;
      return 0;
    }
  }

  frame->parent = forge->stack;
  frame->ref    = ref;

  if (ref) {
    if (!lv2_atom_forge_deref(forge, ref)) {
      frame->ref = 0;
      return 0;
    }

    forge->stack = frame; // Don't push, so walking the stack is always safe
    forge->status = LV2_ATOM_FORGE_SUCCESS;
  } else if (forge->status == LV2_ATOM_FORGE_SUCCESS) {
    forge->status = LV2_ATOM_FORGE_ERR_NO_SPACE;
  }

  return ref;
}

/** Initialise a frame as failed before validating a container operation. */
static inline void
lv2_atom_forge_init_frame(LV2_Atom_Forge* forge, LV2_Atom_Forge_Frame* frame)
{
  if (frame) {
    frame->parent = forge ? forge->stack : NULL;
    frame->ref    = 0;
  }
}

/**
   Pop a stack frame, checking that it is the current top of the stack.

   This function never modifies the stack on failure, so the caller can recover
   by popping the correct frame.

   @return True on success, otherwise false.
*/
static inline bool
lv2_atom_forge_pop_checked(LV2_Atom_Forge* forge, LV2_Atom_Forge_Frame* frame)
{
  if (!forge || !frame || !frame->ref) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    }
    return false;
  }

  if (frame != forge->stack) {
    forge->status = LV2_ATOM_FORGE_ERR_INVALID_LIFECYCLE;
    return false;
  }

  const LV2_Atom_Forge_Frame* parent = frame->parent;
  if (parent) {
    const LV2_Atom_Forge_Frame* f = forge->stack;
    while (f && f != parent) {
      f = f->parent;
    }

    if (!f) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_LIFECYCLE;
      return false;
    }
  }

  forge->stack = frame->parent;
  forge->status = LV2_ATOM_FORGE_SUCCESS;
  return true;
}

/**
   Pop a stack frame.  This must be called when a container is finished.

   This compatibility wrapper is safe when called out of order: it does
   nothing rather than corrupting the stack.  Use
   lv2_atom_forge_pop_checked() to detect this case.
*/
static inline void
lv2_atom_forge_pop(LV2_Atom_Forge* forge, LV2_Atom_Forge_Frame* frame)
{
  (void)lv2_atom_forge_pop_checked(forge, frame);
}

/**
   Abandon all open containers after an error.

   This discards the frame stack and clears the error status, but leaves the
   output position untouched.  Use lv2_atom_forge_set_buffer() to also rewind
   and start a new graph.
*/
static inline void
lv2_atom_forge_recover(LV2_Atom_Forge* forge)
{
  if (forge) {
    forge->stack  = NULL;
    forge->status = LV2_ATOM_FORGE_SUCCESS;
  }
}

/** Return true iff the top of the stack has the given type. */
static inline bool
lv2_atom_forge_top_is(LV2_Atom_Forge* forge, uint32_t type)
{
  if (!forge || !forge->stack || !forge->stack->ref) {
    return false;
  }

  const LV2_Atom* atom = lv2_atom_forge_deref(forge, forge->stack->ref);
  return atom && atom->type == type;
}

/** Return true iff `type` is an atom:Object. */
static inline bool
lv2_atom_forge_is_object_type(const LV2_Atom_Forge* forge, uint32_t type)
{
  return forge && (type == forge->Object || type == forge->Blank ||
                   type == forge->Resource);
}

/** Return true iff `type` is an atom:Object with a blank ID. */
static inline bool
lv2_atom_forge_is_blank(const LV2_Atom_Forge*       forge,
                        uint32_t                    type,
                        const LV2_Atom_Object_Body* body)
{
  return forge && body &&
         (type == forge->Blank || (type == forge->Object && body->id == 0));
}

/**
   @}
   @name Output Configuration
   @{
*/

/** Set the output buffer where `forge` will write atoms. */
static inline void
lv2_atom_forge_set_buffer(LV2_Atom_Forge* forge, uint8_t* buf, size_t size)
{
  if (!forge) {
    return;
  }

  if (size > UINT32_MAX) {
    forge->buf    = NULL;
    forge->size   = 0;
    forge->offset = 0;
    forge->deref  = NULL;
    forge->sink   = NULL;
    forge->handle = NULL;
    forge->stack  = NULL;
    forge->status = LV2_ATOM_FORGE_ERR_INVALID_INDEX;
    return;
  }

  forge->buf    = buf;
  forge->size   = (uint32_t)size;
  forge->offset = 0;
  forge->deref  = NULL;
  forge->sink   = NULL;
  forge->handle = NULL;
  forge->stack  = NULL;
  forge->status = LV2_ATOM_FORGE_SUCCESS;
}

/**
   Set the sink function where `forge` will write output.

   The return value of forge functions is an LV2_Atom_Forge_Ref which is an
   integer type safe to use as a pointer but is otherwise opaque.  The sink
   function must return a ref that can be dereferenced to access as least
   sizeof(LV2_Atom) bytes of the written data, so sizes can be updated.  For
   ringbuffers, this should be possible as long as the size of the buffer is a
   multiple of sizeof(LV2_Atom), since atoms are always aligned.

   Note that 0 is an invalid reference, so if you are using a buffer offset be
   sure to offset it such that 0 is never a valid reference.  You will get
   confusing errors otherwise.
*/
static inline void
lv2_atom_forge_set_sink(LV2_Atom_Forge*            forge,
                        LV2_Atom_Forge_Sink        sink,
                        LV2_Atom_Forge_Deref_Func  deref,
                        LV2_Atom_Forge_Sink_Handle handle)
{
  if (!forge) {
    return;
  }

  forge->buf  = NULL;
  forge->size = forge->offset = 0;
  forge->deref                = deref;
  forge->sink                 = sink;
  forge->handle               = handle;
  forge->stack                = NULL;
  forge->status               = (sink && deref)
                                  ? LV2_ATOM_FORGE_SUCCESS
                                  : LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
}

/**
   @}
   @name Low Level Output
   @{
*/

/**
   Write raw output.  This is used internally, but is also useful for writing
   atom types not explicitly supported by the forge API.  Note the caller is
   responsible for ensuring the output is appropriately padded.
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_raw(LV2_Atom_Forge* forge, const void* data, uint32_t size)
{
  if (!forge || (size && !data)) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    }
    return 0;
  }

  for (const LV2_Atom_Forge_Frame* f = forge->stack; f; f = f->parent) {
    const LV2_Atom* atom = f->ref ? lv2_atom_forge_deref(forge, f->ref) : NULL;
    if (!atom) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_LIFECYCLE;
      return 0;
    }

    if (UINT32_MAX - atom->size < size) {
      forge->status = LV2_ATOM_FORGE_ERR_OVERFLOW;
      return 0;
    }
  }

  LV2_Atom_Forge_Ref out = 0;
  if (forge->sink) {
    if (!forge->deref) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
      return 0;
    }

    out = forge->sink(forge->handle, data, size);
    if (!out) {
      forge->status = LV2_ATOM_FORGE_ERR_NO_SPACE;
      return 0;
    }
  } else {
    if (!forge->buf) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
      return 0;
    }

    if (forge->offset > forge->size) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_INDEX;
      return 0;
    }

    if (size > forge->size - forge->offset) {
      forge->status = LV2_ATOM_FORGE_ERR_NO_SPACE;
      return 0;
    }

    uint8_t* const mem = forge->buf + forge->offset;
    out                = (LV2_Atom_Forge_Ref)mem;

    if (size) {
      memcpy(mem, data, size);
    }
    forge->offset += size;
  }

  for (const LV2_Atom_Forge_Frame* f = forge->stack; f; f = f->parent) {
    LV2_Atom* const atom = lv2_atom_forge_deref(forge, f->ref);
    if (atom) {
      atom->size += size;
    }
  }

  forge->status = LV2_ATOM_FORGE_SUCCESS;
  return out;
}

/** Pad output accordingly so next write is 64-bit aligned. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_pad(LV2_Atom_Forge* forge, uint32_t written)
{
  if (!forge) {
    return 0;
  }

  if (written > UINT32_MAX - 7U) {
    forge->status = LV2_ATOM_FORGE_ERR_OVERFLOW;
    return 0;
  }

  const uint64_t pad      = 0;
  const uint32_t padded   = (written + 7U) & ~7U;
  const uint32_t pad_size = padded - written;
  const LV2_Atom_Forge_Status status = forge->status;
  const LV2_Atom_Forge_Ref    out =
    lv2_atom_forge_raw(forge, &pad, pad_size);
  if (!out) {
    // Padding is optional at the end of a buffer, so this is not an error.
    forge->status = status;
  }

  return out;
}

/** Write raw output, padding to 64-bits as necessary. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_write(LV2_Atom_Forge* forge, const void* data, uint32_t size)
{
  const LV2_Atom_Forge_Ref out = lv2_atom_forge_raw(forge, data, size);
  if (out) {
    lv2_atom_forge_pad(forge, size);
  }

  return out;
}

/** Write a null-terminated string body. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_string_body(LV2_Atom_Forge* forge, const char* str, uint32_t len)
{
  if (!forge || (len && !str)) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    }
    return 0;
  }

  if (len == UINT32_MAX) {
    forge->status = LV2_ATOM_FORGE_ERR_OVERFLOW;
    return 0;
  }

  const LV2_Atom_Forge_Ref s = lv2_atom_forge_raw(forge, str, len);
  if (!s) {
    return s;
  }

  const LV2_Atom_Forge_Ref t = lv2_atom_forge_raw(forge, "", 1);
  if (!t) {
    return t;
  }

  lv2_atom_forge_pad(forge, len + 1U);
  return t;
}

/**
   @}
   @name Atom Output
   @{
*/

/** Write an atom:Atom header. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_atom(LV2_Atom_Forge* forge, uint32_t size, uint32_t type)
{
  if (!forge || (size && !type)) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom a = {size, type};
  return lv2_atom_forge_raw(forge, &a, sizeof(a));
}

/** Write a primitive (fixed-size) atom. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_primitive(LV2_Atom_Forge* forge, const LV2_Atom* a)
{
  if (!forge || !a || (a->size && !a->type)) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  return (
    lv2_atom_forge_top_is(forge, forge->Vector)
      ? lv2_atom_forge_raw(forge, LV2_ATOM_BODY_CONST(a), a->size)
      : lv2_atom_forge_write(forge, a, (uint32_t)sizeof(LV2_Atom) + a->size));
}

/** Write an atom:Int. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_int(LV2_Atom_Forge* forge, int32_t val)
{
  const LV2_Atom_Int a = {{sizeof(val), forge->Int}, val};
  return lv2_atom_forge_primitive(forge, (const LV2_Atom*)&a);
}

/** Write an atom:Long. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_long(LV2_Atom_Forge* forge, int64_t val)
{
  const LV2_Atom_Long a = {{sizeof(val), forge->Long}, val};
  return lv2_atom_forge_primitive(forge, (const LV2_Atom*)&a);
}

/** Write an atom:Float. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_float(LV2_Atom_Forge* forge, float val)
{
  const LV2_Atom_Float a = {{sizeof(val), forge->Float}, val};
  return lv2_atom_forge_primitive(forge, (const LV2_Atom*)&a);
}

/** Write an atom:Double. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_double(LV2_Atom_Forge* forge, double val)
{
  const LV2_Atom_Double a = {{sizeof(val), forge->Double}, val};
  return lv2_atom_forge_primitive(forge, (const LV2_Atom*)&a);
}

/** Write an atom:Bool. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_bool(LV2_Atom_Forge* forge, bool val)
{
  const LV2_Atom_Bool a = {{sizeof(int32_t), forge->Bool}, val ? 1 : 0};
  return lv2_atom_forge_primitive(forge, (const LV2_Atom*)&a);
}

/** Write an atom:URID. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_urid(LV2_Atom_Forge* forge, LV2_URID id)
{
  if (!forge || !id) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_URID a = {{sizeof(id), forge->URID}, id};
  return lv2_atom_forge_primitive(forge, (const LV2_Atom*)&a);
}

/** Write an atom compatible with atom:String.  Used internally. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_typed_string(LV2_Atom_Forge* forge,
                            uint32_t        type,
                            const char*     str,
                            uint32_t        len)
{
  if (!forge || !type || (len && !str)) {
    if (forge) {
      forge->status = type ? LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT
                           : LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  if (len == UINT32_MAX) {
    forge->status = LV2_ATOM_FORGE_ERR_OVERFLOW;
    return 0;
  }

  const LV2_Atom_String a   = {{len + 1, type}};
  LV2_Atom_Forge_Ref    out = lv2_atom_forge_raw(forge, &a, sizeof(a));
  if (out) {
    if (!lv2_atom_forge_string_body(forge, str, len)) {
      LV2_Atom* atom = lv2_atom_forge_deref(forge, out);
      if (atom) {
        atom->size = atom->type = 0;
      }
      out                     = 0;
    }
  }
  return out;
}

/** Write an atom:String.  Note that `str` need not be NULL terminated. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_string(LV2_Atom_Forge* forge, const char* str, uint32_t len)
{
  return lv2_atom_forge_typed_string(forge, forge->String, str, len);
}

/**
   Write an atom:URI.  Note that `uri` need not be NULL terminated.
   This does not map the URI, but writes the complete URI string.  To write
   a mapped URI, use lv2_atom_forge_urid().
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_uri(LV2_Atom_Forge* forge, const char* uri, uint32_t len)
{
  return lv2_atom_forge_typed_string(forge, forge->URI, uri, len);
}

/** Write an atom:Path.  Note that `path` need not be NULL terminated. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_path(LV2_Atom_Forge* forge, const char* path, uint32_t len)
{
  return lv2_atom_forge_typed_string(forge, forge->Path, path, len);
}

/** Write an atom:Literal. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_literal(LV2_Atom_Forge* forge,
                       const char*     str,
                       uint32_t        len,
                       uint32_t        datatype,
                       uint32_t        lang)
{
  if (!forge || (len && !str)) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    }
    return 0;
  }

  if (len == UINT32_MAX) {
    forge->status = LV2_ATOM_FORGE_ERR_OVERFLOW;
    return 0;
  }

  const LV2_Atom_Literal a = {
    {(uint32_t)(sizeof(LV2_Atom_Literal) - sizeof(LV2_Atom) + len + 1),
     forge->Literal},
    {datatype, lang}};
  LV2_Atom_Forge_Ref out = lv2_atom_forge_raw(forge, &a, sizeof(a));
  if (out) {
    if (!lv2_atom_forge_string_body(forge, str, len)) {
      LV2_Atom* atom = lv2_atom_forge_deref(forge, out);
      if (atom) {
        atom->size = atom->type = 0;
      }
      out                     = 0;
    }
  }
  return out;
}

/** Start an atom:Vector. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_vector_head(LV2_Atom_Forge*       forge,
                           LV2_Atom_Forge_Frame* frame,
                           uint32_t              child_size,
                           uint32_t              child_type)
{
  lv2_atom_forge_init_frame(forge, frame);
  if (!forge || !frame || !child_size || !child_type) {
    if (forge) {
      forge->status = child_type ? LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT
                                 : LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_Vector a = {{sizeof(LV2_Atom_Vector_Body), forge->Vector},
                             {child_size, child_type}};
  return lv2_atom_forge_push(
    forge, frame, lv2_atom_forge_write(forge, &a, sizeof(a)));
}

/** Write a complete atom:Vector. */
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_vector(LV2_Atom_Forge* forge,
                      uint32_t        child_size,
                      uint32_t        child_type,
                      uint32_t        n_elems,
                      const void*     elems)
{
  if (!forge || !child_size || !child_type || (n_elems && !elems)) {
    if (forge) {
      forge->status = child_type ? LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT
                                 : LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  if (n_elems && child_size > UINT32_MAX / n_elems) {
    forge->status = LV2_ATOM_FORGE_ERR_OVERFLOW;
    return 0;
  }

  const LV2_Atom_Vector a = {
    {(uint32_t)sizeof(LV2_Atom_Vector_Body) + (n_elems * child_size),
     forge->Vector},
    {child_size, child_type}};
  const LV2_Atom_Forge_Ref out = lv2_atom_forge_write(forge, &a, sizeof(a));
  if (out) {
    lv2_atom_forge_write(forge, elems, child_size * n_elems);
  }
  return out;
}

/**
   Write the header of an atom:Tuple.

   The passed frame will be initialised to represent this tuple.  To complete
   the tuple, write a sequence of atoms, then pop the frame with
   lv2_atom_forge_pop().

   For example:
   @code
   // Write tuple (1, 2.0)
   LV2_Atom_Forge_Frame frame;
   LV2_Atom* tup = (LV2_Atom*)lv2_atom_forge_tuple(forge, &frame);
   lv2_atom_forge_int(forge, 1);
   lv2_atom_forge_float(forge, 2.0);
   lv2_atom_forge_pop(forge, &frame);
   @endcode
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_tuple(LV2_Atom_Forge* forge, LV2_Atom_Forge_Frame* frame)
{
  lv2_atom_forge_init_frame(forge, frame);
  if (!forge || !frame || !forge->Tuple) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_Tuple a = {{0, forge->Tuple}};
  return lv2_atom_forge_push(
    forge, frame, lv2_atom_forge_write(forge, &a, sizeof(a)));
}

/**
   Write the header of an atom:Object.

   The passed frame will be initialised to represent this object.  To complete
   the object, write a sequence of properties, then pop the frame with
   lv2_atom_forge_pop().

   For example:
   @code
   LV2_URID eg_Cat  = map("http://example.org/Cat");
   LV2_URID eg_name = map("http://example.org/name");

   // Start object with type eg_Cat and blank ID
   LV2_Atom_Forge_Frame frame;
   lv2_atom_forge_object(forge, &frame, 0, eg_Cat);

   // Append property eg:name = "Hobbes"
   lv2_atom_forge_key(forge, eg_name);
   lv2_atom_forge_string(forge, "Hobbes", strlen("Hobbes"));

   // Finish object
   lv2_atom_forge_pop(forge, &frame);
   @endcode
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_object(LV2_Atom_Forge*       forge,
                      LV2_Atom_Forge_Frame* frame,
                      LV2_URID              id,
                      LV2_URID              otype)
{
  lv2_atom_forge_init_frame(forge, frame);
  if (!forge || !frame || !otype || !forge->Object) {
    if (forge) {
      forge->status = otype ? LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT
                            : LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_Object a = {
    {(uint32_t)sizeof(LV2_Atom_Object_Body), forge->Object}, {id, otype}};
  return lv2_atom_forge_push(
    forge, frame, lv2_atom_forge_write(forge, &a, sizeof(a)));
}

/**
   The same as lv2_atom_forge_object(), but for object:Resource.

   This function is deprecated and should not be used in new code.
   Use lv2_atom_forge_object() directly instead.
*/
LV2_DEPRECATED static inline LV2_Atom_Forge_Ref
lv2_atom_forge_resource(LV2_Atom_Forge*       forge,
                        LV2_Atom_Forge_Frame* frame,
                        LV2_URID              id,
                        LV2_URID              otype)
{
  lv2_atom_forge_init_frame(forge, frame);
  if (!forge || !frame || !id || !otype || !forge->Resource) {
    if (forge) {
      forge->status = (!id || !otype || !forge->Resource)
                        ? LV2_ATOM_FORGE_ERR_INVALID_RESOURCE
                        : LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT;
    }
    return 0;
  }

  const LV2_Atom_Object a = {
    {(uint32_t)sizeof(LV2_Atom_Object_Body), forge->Resource}, {id, otype}};
  return lv2_atom_forge_push(
    forge, frame, lv2_atom_forge_write(forge, &a, sizeof(a)));
}

/**
   The same as lv2_atom_forge_object(), but for object:Blank.

   This function is deprecated and should not be used in new code.
   Use lv2_atom_forge_object() directly instead.
*/
LV2_DEPRECATED static inline LV2_Atom_Forge_Ref
lv2_atom_forge_blank(LV2_Atom_Forge*       forge,
                     LV2_Atom_Forge_Frame* frame,
                     uint32_t              id,
                     LV2_URID              otype)
{
  lv2_atom_forge_init_frame(forge, frame);
  if (!forge || !frame || !otype || !forge->Blank) {
    if (forge) {
      forge->status = otype ? LV2_ATOM_FORGE_ERR_INVALID_ARGUMENT
                            : LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_Object a = {
    {(uint32_t)sizeof(LV2_Atom_Object_Body), forge->Blank}, {id, otype}};
  return lv2_atom_forge_push(
    forge, frame, lv2_atom_forge_write(forge, &a, sizeof(a)));
}

/**
   Write a property key in an Object, to be followed by the value.

   See lv2_atom_forge_object() documentation for an example.
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_key(LV2_Atom_Forge* forge, LV2_URID key)
{
  if (!forge || !key) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_Property_Body a = {key, 0, {0, 0}};
  return lv2_atom_forge_write(forge, &a, 2 * (uint32_t)sizeof(uint32_t));
}

/**
   Write the header for a property body in an object, with context.

   If you do not need the context, which is almost certainly the case,
   use the simpler lv2_atom_forge_key() instead.
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_property_head(LV2_Atom_Forge* forge,
                             LV2_URID        key,
                             LV2_URID        context)
{
  if (!forge || !key) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_Property_Body a = {key, context, {0, 0}};
  return lv2_atom_forge_write(forge, &a, 2 * (uint32_t)sizeof(uint32_t));
}

/**
   Write the header for a Sequence.
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_sequence_head(LV2_Atom_Forge*       forge,
                             LV2_Atom_Forge_Frame* frame,
                             uint32_t              unit)
{
  lv2_atom_forge_init_frame(forge, frame);
  if (!forge || !frame || !forge->Sequence) {
    if (forge) {
      forge->status = LV2_ATOM_FORGE_ERR_INVALID_RESOURCE;
    }
    return 0;
  }

  const LV2_Atom_Sequence a = {
    {(uint32_t)sizeof(LV2_Atom_Sequence_Body), forge->Sequence}, {unit, 0}};
  return lv2_atom_forge_push(
    forge, frame, lv2_atom_forge_write(forge, &a, sizeof(a)));
}

/**
   Write the time stamp header of an Event (in a Sequence) in audio frames.
   After this, call the appropriate forge method(s) to write the body.  Note
   the returned reference is to an LV2_Event which is NOT an Atom.
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_frame_time(LV2_Atom_Forge* forge, int64_t frames)
{
  return lv2_atom_forge_write(forge, &frames, sizeof(frames));
}

/**
   Write the time stamp header of an Event (in a Sequence) in beats.  After
   this, call the appropriate forge method(s) to write the body.  Note the
   returned reference is to an LV2_Event which is NOT an Atom.
*/
static inline LV2_Atom_Forge_Ref
lv2_atom_forge_beat_time(LV2_Atom_Forge* forge, double beats)
{
  return lv2_atom_forge_write(forge, &beats, sizeof(beats));
}

LV2_RESTORE_WARNINGS

#ifdef __cplusplus
} /* extern "C" */
#endif

/**
   @}
   @}
*/

#endif // LV2_ATOM_FORGE_H
