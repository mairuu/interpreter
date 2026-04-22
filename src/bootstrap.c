#include "bootstrap.h"

#include "object.h"
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
  BUILTIN_COUNT,
} BuiltinBindTarget;

#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof(arr[0]))

#define GET_SELF_ARRAY(self_var, array_var)                                    \
  Value self_var = args[-1];                                                   \
  ObjectArray *array_var = (ObjectArray *)AS_TRAIT_OBJECT(self_var)->receiver

#define GET_SELF_ITERATOR(self_var, iter_var)                                  \
  Value self_var = args[-1];                                                   \
  ObjectArrayIterator *iter_var =                                              \
      (ObjectArrayIterator *)AS_TRAIT_OBJECT(self_var)->receiver

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
  default:
    vm_runtime_error(vm, "__builtin_bind: unknown target id %d", target);
  }

  return NIL_VALUE;
}

static bool implement_array_for_obj_array(VirtualMachine *vm);
static bool implement_into_iterable_for_obj_array(VirtualMachine *vm);
static bool implement_iterable_for_obj_array_iterator(VirtualMachine *vm);

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
  };
  for (size_t i = 0; i < ARRAY_COUNT(constants); i++) {
    vm_define_global(vm, constants[i].name, NUMBER_VALUE(constants[i].value));
  }

  const char *sources[] = {PRELUDE_ITERABLE, PRELUDE_RESULT, PRELUDE_ARRAY};
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
  GET_SELF_ITERATOR(self, iter);
  return BOOL_VALUE(iter->index < iter->array->length);
}

Value array_iterator_next(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 0) {
    vm_runtime_error(vm, "next: expected 0 args");
    return NIL_VALUE;
  }
  GET_SELF_ITERATOR(self, iter);
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
