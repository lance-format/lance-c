/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file attributes.h
 * @brief Declaration attributes shared by every public header.
 */

#ifndef LANCE_ATTRIBUTES_H
#define LANCE_ATTRIBUTES_H

/* Marks declarations exported by the shared library. On Windows the default is
 * an ordinary declaration, so one header works with both the static archive
 * and the DLL import library. Define LANCE_C_SHARED when linking the DLL to
 * use __declspec(dllimport), or LANCE_C_BUILDING_LIBRARY when building it. */
#if defined(_WIN32)
#  if defined(LANCE_C_BUILDING_LIBRARY)
#    define LANCE_API __declspec(dllexport)
#  elif defined(LANCE_C_SHARED)
#    define LANCE_API __declspec(dllimport)
#  else
#    define LANCE_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define LANCE_API __attribute__((visibility("default")))
#else
#  define LANCE_API
#endif

/* Compiler-enforced deprecation. The message names the replacement API. */
#if defined(__cplusplus) && __cplusplus >= 201402L
#  define LANCE_DEPRECATED(message) [[deprecated(message)]]
#elif defined(_MSC_VER)
#  define LANCE_DEPRECATED(message) __declspec(deprecated(message))
#elif defined(__GNUC__) || defined(__clang__)
#  define LANCE_DEPRECATED(message) __attribute__((deprecated(message)))
#else
#  define LANCE_DEPRECATED(message)
#endif

/* Marks a result the caller must observe, such as a handle constructor or a
 * borrowed lookup result. */
#if defined(__cplusplus) && __cplusplus >= 201703L
#  define LANCE_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#  define LANCE_NODISCARD __attribute__((warn_unused_result))
#else
#  define LANCE_NODISCARD
#endif

#endif /* LANCE_ATTRIBUTES_H */
