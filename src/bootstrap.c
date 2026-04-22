#include "bootstrap.h"

#include "object.h"
#include "string_utils.h"
#include "value.h"
#include "virtual_machine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef enum {
  BUILTIN_ITERABLE = 0,
  BUILTIN_INTO_ITERABLE,
  BUILTIN_RESULT,
  BUILTIN_ARRAY,
  BUILTIN_MAP,
  BUILTIN_MAP_ENTRY,
  BUILTIN_COUNT,
} BuiltinBindTarget;

#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof(arr[0]))

#define GET_SELF_ARRAY(self_var, array_var)                                    \
  Value self_var = args[-1];                                                   \
  ObjectArray *array_var = (ObjectArray *)AS_TRAIT_OBJECT(self_var)->receiver

#define GET_SELF_ARRAY_ITERATOR(self_var, iter_var)                            \
  Value self_var = args[-1];                                                   \
  ObjectArrayIterator *iter_var =                                              \
      (ObjectArrayIterator *)AS_TRAIT_OBJECT(self_var)->receiver

#define GET_SELF_MAP(self_var, map_var)                                        \
  Value self_var = args[-1];                                                   \
  ObjectMap *map_var = (ObjectMap *)AS_TRAIT_OBJECT(self_var)->receiver

#define GET_SELF_MAP_ITERATOR(self_var, iter_var)                              \
  Value self_var = args[-1];                                                   \
  ObjectMapIterator *iter_var =                                                \
      (ObjectMapIterator *)AS_TRAIT_OBJECT(self_var)->receiver

static const char *PRELUDE_ITERABLE =
    "trait Iterable { has_next next }\n"
    "__builtin_bind(__BUILTIN_ITERABLE, Iterable)\n"
    "trait IntoIterable { iter }\n"
    "__builtin_bind(__BUILTIN_INTO_ITERABLE, IntoIterable)\n"
    "fun iter(v) {"
    " return match v {"
    "   Iterable(t) => t "
    "   IntoIterable(t) => t.iter() as Iterable "
    "   _ => panic(v, \"is not iterable\") "
    " }"
    "}\n";

static const char *PRELUDE_RESULT =
    "variant Result { Ok(v) Err(msg) }\n"
    "__builtin_bind(__BUILTIN_RESULT, Result)\n";

static const char *PRELUDE_ARRAY = "trait Array { push pop get set length }\n"
                                   "__builtin_bind(__BUILTIN_ARRAY, Array)\n";

static const char *PRELUDE_MAP =
    "trait Map { get set delete has length }\n"
    "__builtin_bind(__BUILTIN_MAP, Map)\n"
    "struct MapEntry { key value }\n"
    "__builtin_bind(__BUILTIN_MAP_ENTRY, MapEntry)\n";

typedef struct {
  const char *name;
  const char *source;
} PreludeEntry;

static const PreludeEntry MANIFEST[] = {
    {"iterable.dt", NULL},
    {"result.dt", NULL},
    {"array.dt", NULL},
    {"map.dt", NULL},
};

Value native_builtin_bind(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 2) {
    vm_runtime_error(vm, "__builtin_bind: expected 2 args (target_id, value)");
  }
  if (!IS_NUMBER(args[0])) {
    vm_runtime_error(vm, "__builtin_bind: first arg must be a bind-target id");
  }

  int target = (int)AS_NUMBER(args[0]);

  switch ((BuiltinBindTarget)target) {
  case BUILTIN_ITERABLE:
    if (!IS_TRAIT_DEFINITION(args[1])) {
      vm_runtime_error(
          vm, "__builtin_bind: BUILTIN_ITERABLE expects a trait definition");
    }
    vm->builtins.iterable = AS_TRAIT_DEFINITION(args[1]);
    break;
  case BUILTIN_RESULT:
    if (!IS_VARIANT_DEFINITION(args[1])) {
      vm_runtime_error(
          vm, "__builtin_bind: BUILTIN_RESULT expects a variant definition");
    }
    vm->builtins.result = AS_VARIANT_DEFINITION(args[1]);
    break;
  case BUILTIN_INTO_ITERABLE:
    if (!IS_TRAIT_DEFINITION(args[1])) {
      vm_runtime_error(
          vm,
          "__builtin_bind: BUILTIN_INTO_ITERABLE expects a trait definition");
    }
    vm->builtins.into_iterable = AS_TRAIT_DEFINITION(args[1]);
    break;
  case BUILTIN_ARRAY:
    if (!IS_TRAIT_DEFINITION(args[1])) {
      vm_runtime_error(
          vm, "__builtin_bind: BUILTIN_ARRAY expects a trait definition");
    }
    vm->builtins.array = AS_TRAIT_DEFINITION(args[1]);
    break;
  case BUILTIN_MAP:
    if (!IS_TRAIT_DEFINITION(args[1])) {
      vm_runtime_error(
          vm, "__builtin_bind: BUILTIN_MAP expects a trait definition");
    }
    vm->builtins.map = AS_TRAIT_DEFINITION(args[1]);
    break;
  case BUILTIN_MAP_ENTRY:
    if (!IS_STRUCT_DEFINITION(args[1])) {
      vm_runtime_error(
          vm, "__builtin_bind: BUILTIN_MAP_ENTRY expects a struct definition");
    }
    vm->builtins.map_entry = AS_STRUCT_DEFINITION(args[1]);
    break;
  default:
    vm_runtime_error(vm, "__builtin_bind: unknown target id %d", target);
  }

  return NIL_VALUE;
}

static bool implement_array_for_obj_array(VirtualMachine *vm);
static bool implement_into_iterable_for_obj_array(VirtualMachine *vm);
static bool implement_iterable_for_obj_array_iterator(VirtualMachine *vm);
static bool implement_map_for_obj_map(VirtualMachine *vm);
static bool implement_into_iterable_for_obj_map(VirtualMachine *vm);
static bool implement_iterable_for_obj_map_iterator(VirtualMachine *vm);

bool bootstrap(VirtualMachine *vm) {
  vm_define_native(vm, "__builtin_bind", native_builtin_bind);

  struct {
    const char *name;
    double value;
  } constants[] = {
      {"__BUILTIN_ITERABLE", (double)BUILTIN_ITERABLE},
      {"__BUILTIN_INTO_ITERABLE", (double)BUILTIN_INTO_ITERABLE},
      {"__BUILTIN_RESULT", (double)BUILTIN_RESULT},
      {"__BUILTIN_ARRAY", (double)BUILTIN_ARRAY},
      {"__BUILTIN_MAP", (double)BUILTIN_MAP},
      {"__BUILTIN_MAP_ENTRY", (double)BUILTIN_MAP_ENTRY},
  };
  for (size_t i = 0; i < ARRAY_COUNT(constants); i++) {
    vm_define_global(vm, constants[i].name, NUMBER_VALUE(constants[i].value));
  }

  const char *sources[] = {PRELUDE_ITERABLE, PRELUDE_RESULT, PRELUDE_ARRAY,
                           PRELUDE_MAP};
  for (size_t i = 0; i < ARRAY_COUNT(sources); i++) {
    InterpretResult result = vm_interpret(vm, sources[i]);
    if (result != INTERPRET_OK) {
      fprintf(stderr, "bootstrap: failed to load prelude '%s'\n",
              MANIFEST[i].name);
      return false;
    }
  }

  struct {
    void *ptr;
    const char *name;
  } builtins_check[] = {
      {vm->builtins.iterable, "iterable"},
      {vm->builtins.into_iterable, "into_iterable"},
      {vm->builtins.result, "result"},
      {vm->builtins.array, "array"},
      {vm->builtins.map, "map"},
  };
  for (size_t i = 0; i < ARRAY_COUNT(builtins_check); i++) {
    if (builtins_check[i].ptr == NULL) {
      fprintf(stderr,
              "bootstrap: vm->builtins.%s was not set — did the prelude "
              "call __builtin_bind?\n",
              builtins_check[i].name);
      return false;
    }
  }

  vm_undefine_global(vm, "__builtin_bind");

  struct {
    bool (*impl_fn)(VirtualMachine *);
    const char *name;
  } implementations[] = {
      {implement_array_for_obj_array, "Array for ObjectArray"},
      {implement_into_iterable_for_obj_array, "IntoIterable for ObjectArray"},
      {implement_iterable_for_obj_array_iterator,
       "Iterable for ObjectArrayIterator"},
      {implement_map_for_obj_map, "Map for ObjectMap"},
      {implement_into_iterable_for_obj_map, "IntoIterable for ObjectMap"},
      {implement_iterable_for_obj_map_iterator,
       "Iterable for ObjectMapIterator"},
  };
  for (size_t i = 0; i < ARRAY_COUNT(implementations); i++) {
    if (!implementations[i].impl_fn(vm)) {
      fprintf(stderr, "bootstrap: failed to implement %s\n",
              implementations[i].name);
      return false;
    }
  }

  return true;
}

static Value array_push(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 1) {
    vm_runtime_error(vm, "push: expected 1 arg (value)");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY(self, array);
  if (!obj_array_push(array, args[0], &vm->al)) {
    vm_runtime_error(vm, "failed to push to array");
    return NIL_VALUE;
  }
  return NIL_VALUE;
}

static Value array_pop(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "pop: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY(self, array);
  return obj_array_pop(array);
}

static Value array_get(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 1) {
    vm_runtime_error(vm, "get: expected 1 arg (index)");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY(self, array);
  if (!IS_NUMBER(args[0])) {
    vm_runtime_error(vm, "get: index must be a number");
    return NIL_VALUE;
  }
  int index = (int)AS_NUMBER(args[0]);
  Value r = obj_array_get(array, index);
  if (IS_EMPTY(r)) {
    vm_runtime_error(vm, "get: index out of bounds");
    return NIL_VALUE;
  }
  return r;
}

static Value array_set(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 2) {
    vm_runtime_error(vm, "set: expected 2 args (index, value)");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY(self, array);
  if (!IS_NUMBER(args[0])) {
    vm_runtime_error(vm, "set: index must be a number");
    return NIL_VALUE;
  }
  int index = (int)AS_NUMBER(args[0]);
  if (!obj_array_set(array, index, args[1])) {
    vm_runtime_error(vm, "set: index out of bounds");
    return NIL_VALUE;
  }
  return NIL_VALUE;
}

static Value array_length(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "length: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY(self, array);
  return NUMBER_VALUE((double)array->length);
}

static bool implement_array_for_obj_array(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {"push", array_push}, {"pop", array_pop},       {"get", array_get},
      {"set", array_set},   {"length", array_length},
  };
  ObjectImpl *impl = NULL;
  bool ok = vm_register_native_impl(vm, OBJECT_ARRAY, vm->builtins.array,
                                    methods, ARRAY_COUNT(methods), &impl);
  if (ok) {
    vm->builtins.array_impl_obj_array = impl;
  }
  return ok;
}

static Value array_iter(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "iter: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY(self, array);
  ObjectArrayIterator *iter = vm_new_array_iterator(vm, array);
  return OBJECT_VALUE(iter);
}

static bool implement_into_iterable_for_obj_array(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {"iter", array_iter},
  };
  return vm_register_native_impl(vm, OBJECT_ARRAY, vm->builtins.into_iterable,
                                 methods, ARRAY_COUNT(methods), NULL);
}

Value array_iterator_has_next(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "has_next: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY_ITERATOR(self, iter);
  return BOOL_VALUE(iter->index < iter->array->length);
}

Value array_iterator_next(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "next: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_ARRAY_ITERATOR(self, iter);
  if (iter->index >= iter->array->length) {
    vm_runtime_error(vm, "next: no more elements");
    return NIL_VALUE;
  }
  Value value = iter->array->values[iter->index];
  iter->index++;
  return value;
}

static bool implement_iterable_for_obj_array_iterator(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {"has_next", array_iterator_has_next},
      {"next", array_iterator_next},
  };
  return vm_register_native_impl(vm, OBJECT_ARRAY_ITERATOR,
                                 vm->builtins.iterable, methods,
                                 ARRAY_COUNT(methods), NULL);
}

static Value __make_ok(VirtualMachine *vm, Value val) {
  ObjectVariant *variant = vm_new_variant(vm, vm->builtins.result, 0, 1);
  variant->payload[0] = val;
  return OBJECT_VALUE(variant);
}

static Value __make_err(VirtualMachine *vm, Value val) {
  ObjectVariant *variant = vm_new_variant(vm, vm->builtins.result, 1, 1);
  variant->payload[0] = val;
  return OBJECT_VALUE(variant);
}

static Value __make_err_msg(VirtualMachine *vm, const char *msg) {
  vm_begin_staging(vm);
  int length = strlen(msg);
  char *buf = copy_string(msg, length, &vm->al);
  uint32_t hash = hash_string(msg, length);
  ObjectString *err = vm_new_string(vm, buf, length, hash);
  vm_end_staging(vm);
  return __make_err(vm, OBJECT_VALUE(err));
}

static Value map_get(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 1) {
    vm_runtime_error(vm, "get: expected 1 arg (key)");
    return NIL_VALUE;
  }
  GET_SELF_MAP(self, map);
  Value found = obj_map_get(map, args[0]);
  if (IS_EMPTY(found)) {
    return __make_err(vm, __make_err_msg(vm, "key not found"));
  }
  return __make_ok(vm, found);
}

static Value map_set(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 2) {
    vm_runtime_error(vm, "set: expected 2 args (key, value)");
    return NIL_VALUE;
  }
  GET_SELF_MAP(self, map);
  obj_map_set(map, args[0], args[1], &vm->al);
  return NIL_VALUE;
}

static Value map_delete(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 1) {
    vm_runtime_error(vm, "delete: expected 1 arg (key)");
    return NIL_VALUE;
  }
  GET_SELF_MAP(self, map);
  obj_map_delete(map, args[0]);
  return NIL_VALUE;
}

static Value map_has(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 1) {
    vm_runtime_error(vm, "has: expected 1 arg (key)");
    return NIL_VALUE;
  }
  GET_SELF_MAP(self, map);
  return BOOL_VALUE(!IS_EMPTY(obj_map_get(map, args[0])));
}

static Value map_length(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 0) {
    vm_runtime_error(vm, "length: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_MAP(self, map);
  return NUMBER_VALUE((double)map->table.alive);
}

static bool implement_map_for_obj_map(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {"get", map_get}, {"set", map_set},       {"delete", map_delete},
      {"has", map_has}, {"length", map_length},
  };
  ObjectImpl *impl = NULL;
  bool ok = vm_register_native_impl(vm, OBJECT_MAP, vm->builtins.map, methods,
                                    ARRAY_COUNT(methods), &impl);
  if (ok) {
    vm->builtins.map_impl_obj_map = impl;
  }
  return ok;
}

static Value map_iter(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 0) {
    vm_runtime_error(vm, "iter: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_MAP(self, map);

  vm_begin_staging(vm);
  ObjectArray *keys = vm_new_array(vm);
  obj_array_reserve(keys, map->table.count, &vm->al);

  HashTableIterator it;
  hti_init(&it, &map->table);
  while (hti_next(&it)) {
    obj_array_push(keys, *it.key, &vm->al);
  }

  ObjectMapIterator *iter = vm_new_map_iterator(vm, map, keys);
  vm_end_staging(vm);
  return OBJECT_VALUE(iter);
}

static bool implement_into_iterable_for_obj_map(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {"iter", map_iter},
  };
  return vm_register_native_impl(vm, OBJECT_MAP, vm->builtins.into_iterable,
                                 methods, ARRAY_COUNT(methods), NULL);
}

Value map_iterator_has_next(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "has_next: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_MAP_ITERATOR(self, iter);
  return BOOL_VALUE(iter->index < iter->keys->length);
}

Value map_iterator_next(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "next: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_MAP_ITERATOR(self, iter);
  if (iter->index >= iter->keys->length) {
    vm_runtime_error(vm, "next: no more elements");
    return NIL_VALUE;
  }
  Value key = iter->keys->values[iter->index];
  iter->index++;
  Value value = obj_map_get(iter->map, key);
  if (IS_EMPTY(value)) {
    vm_runtime_error(vm,
                     "next: key not found in map (concurrent modification?)");
    return NIL_VALUE;
  }

  ObjectStructInstance *entry =
      vm_new_struct_instance(vm, vm->builtins.map_entry);
  entry->fields[0] = key;
  entry->fields[1] = value;
  return OBJECT_VALUE(entry);
}

static bool implement_iterable_for_obj_map_iterator(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {"has_next", map_iterator_has_next},
      {"next", map_iterator_next},
  };
  return vm_register_native_impl(vm, OBJECT_MAP_ITERATOR, vm->builtins.iterable,
                                 methods, ARRAY_COUNT(methods), NULL);
}