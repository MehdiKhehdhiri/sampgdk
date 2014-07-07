/* Copyright (C) 2012-2014 Zeex
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "array.h"
#include "fakeamx.h"
#include "init.h"
#include "native.h"
#include "log.h"

#define MAX_NATIVE_ARGS     32
#define MAX_NATIVE_ARG_SIZE 8  /* in bytes */

static struct sampgdk_array natives;

SAMPGDK_MODULE_INIT(native) {
  int error;

  error = sampgdk_array_new(&natives, 100, sizeof(AMX_NATIVE_INFO));
  if (error < 0) {
    return error;
  }

  return 0;
}

SAMPGDK_MODULE_CLEANUP(native) {
  sampgdk_array_free(&natives);
}

int sampgdk_native_register(const char *name, AMX_NATIVE func) {
  AMX_NATIVE_INFO info;
  AMX_NATIVE_INFO *ptr;
  int index;

  info.name = name;
  info.func = func;

  assert(name != 0);

  /* Maintain element order (by name). */
  for (index = 0; index < natives.count; index++) {
    ptr = (AMX_NATIVE_INFO *)sampgdk_array_get(&natives, index);
    if (strcmp(ptr->name, name) >= 0) {
      return sampgdk_array_insert_single(&natives, index, &info);
    }
  }

  return sampgdk_array_append(&natives, &info);
}

static int compare(const void *key, const void *elem) {
  assert(key != NULL);
  assert(elem != NULL);
  return strcmp((const char *)key, ((const AMX_NATIVE_INFO *)elem)->name);
}

AMX_NATIVE sampgdk_native_find(const char *name) {
  AMX_NATIVE_INFO *info;

  assert(name != NULL);

  if (natives.data == NULL) {
    /* Perhaps they forgot to initialize? */
    return NULL;
  }

  info = bsearch(name, natives.data, natives.count,
                 natives.elem_size, compare);
  if (info == NULL) {
    return NULL;
  }

  return info->func;
}

AMX_NATIVE sampgdk_native_find_warn(const char *name) {
  AMX_NATIVE func;

  assert(name != NULL);

  func = sampgdk_native_find(name);
  if (func == NULL) {
    sampgdk_log_warn("Native function not found: %s", name);
  }

  return func;
}

static cell AMX_NATIVE_CALL native_stub(AMX *amx, cell *params) {
  sampgdk_log_warn("Native stub");
  return 0;
}

AMX_NATIVE sampgdk_native_find_stub(const char *name) {
  AMX_NATIVE func;

  assert(name != NULL);

  if ((func = sampgdk_native_find(name)) == NULL) {
    return native_stub;
  }

  return func;
}

AMX_NATIVE sampgdk_native_find_warn_stub(const char *name) {
  AMX_NATIVE func;

  assert(name != NULL);

  if ((func = sampgdk_native_find_warn(name)) == NULL) {
    return native_stub;
  }

  return func;
}

const AMX_NATIVE_INFO *sampgdk_native_get_table(int *number) {
  *number = natives.count;
  return (const AMX_NATIVE_INFO*)natives.data;
}

cell sampgdk_native_call(AMX_NATIVE native, cell *params) {
  AMX *amx = sampgdk_fakeamx_amx();
  return native(amx, params);
}

cell sampgdk_native_invoke(AMX_NATIVE native, const char *format,
                           va_list args) {
  cell i;
  unsigned char args_copy[MAX_NATIVE_ARGS * MAX_NATIVE_ARG_SIZE];
  unsigned char *args_ptr = args_copy;
  void *args_array[MAX_NATIVE_ARGS];

  for (i = 0; format[i] != '\0' && i < MAX_NATIVE_ARGS; i++) {
    switch (format[i]) {
      case 'i': /* integer */
      case 'd': /* integer */
        *(int *)args_ptr = va_arg(args, int);
        args_array[i] = args_ptr;
        args_ptr += MAX_NATIVE_ARG_SIZE;
        break;
      case 'b': /* boolean */
        *(bool *)args_ptr = !!va_arg(args, int);
        args_array[i] = args_ptr;
        args_ptr += MAX_NATIVE_ARG_SIZE;
        break;
      case 'f': /* floating-point */
        *(float *)args_ptr = va_arg(args, double);
        args_array[i] = args_ptr;
        args_ptr += MAX_NATIVE_ARG_SIZE;
        break;
      case 'r': /* const reference */
      case 'R': /* non-const reference */
      case 's': /* const string */
      case 'S': /* non-const string */
      case 'a': /* const array */
      case 'A': /* non-const array */
        args_array[i] = va_arg(args, void *);
        break;
      default:
        assert(0 && "Invalid type specifier");
    }
  }

  return sampgdk_native_invoke_array(native, format, args_array);
}

cell sampgdk_native_invoke_array(AMX_NATIVE native, const char *format,
                                 void **args) {
  AMX *amx = sampgdk_fakeamx_amx();
  cell i;
  cell params[MAX_NATIVE_ARGS + 1];
  cell size[MAX_NATIVE_ARGS] = {0};
  int need_size = -1;
  cell retval;

  assert(native != NULL);
  assert(format != NULL);

  for (i = 0; format[i] != '\0' && i < MAX_NATIVE_ARGS; i++) {
    switch (format[i]) {
      case 'i': /* integer */
      case 'd': /* integer */
        params[i + 1] = *(int *)args[i];
        if (need_size >= 0) {
          switch (format[need_size]) {
            case 'a':
            case 'A':
            case 'S':
              sampgdk_fakeamx_push_array(args[need_size], params[i + 1],
                                         &params[need_size]);
              break;
            default:
              assert(0);
          }
          size[need_size] = params[i + 1];
          need_size = -1;
        }
        break;
      case 'b': /* boolean */
        params[i + 1] = *(bool *)args[i];
        break;
      case 'f': /* floating-point */ {
        float value = *(float *)args[i];
        params[i + 1] = amx_ftoc(value);
        break;
      }
      case 'r': /* const reference */
      case 'R': /* non-const reference */ {
        cell *ptr = args[i];
        sampgdk_fakeamx_push_cell(*ptr, &params[i + 1]);
        size[i] = sizeof(cell);
        break;
      }
      case 's': /* const string */ {
        char *str = args[i];
        int str_size;
        sampgdk_fakeamx_push_string(str, &str_size, &params[i + 1]);
        size[i] = str_size;
        break;
      }
      case 'S': /* non-const string */
      case 'a': /* const array */
      case 'A': /* non-const array */
        need_size = i;
        break;
      default:
        assert(0 && "Invalid type specifier");
    }
  }

  assert(need_size == -1 && "Missing string buffer size");

  params[0] = i * sizeof(cell);
  retval = native(amx, params);

  while (--i >= 0) {
    if (size[i] > 0) {
      /* If this is an output parameter we have to write the updated value
       * back to the argument.
       */
      switch (format[i]) {
        case 'R':
          sampgdk_fakeamx_get_cell(params[i + 1], args[i]);
          break;
        case 'S':
          sampgdk_fakeamx_get_string(params[i + 1], args[i], size[i]);
          break;
        case 'A':
          sampgdk_fakeamx_get_array(params[i + 1], args[i], size[i]);
          break;
      }
      sampgdk_fakeamx_pop(params[i + 1]);
    }
  }

  return retval;
}
