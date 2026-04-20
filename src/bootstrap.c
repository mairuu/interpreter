#include "bootstrap.h"

#include "object.h"
#include "string_utils.h"
#include "virtual_machine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef enum {
  BUILTIN_ITERABLE = 0,
  BUILTIN_DESCRIBABLE, // debug
  BUILTIN_COUNT,
} BuiltinBindTarget;

static const char *PRELUDE_ITERABLE =
    "trait Iterable { has_next next }\n"
    "__builtin_bind(__BUILTIN_ITERABLE, Iterable)\n";

static const char *PRELUDE_DESCRIBABLE =
    "trait Describable { describe }\n"
    "__builtin_bind(__BUILTIN_DESCRIBABLE, Describable)\n";

typedef struct {
  const char *name;
  const char *source;
} PreludeEntry;

static const PreludeEntry MANIFEST[] = {
    {"iterable.dt", NULL},
    {"describable.dt", NULL},
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
  case BUILTIN_DESCRIBABLE:
    if (!IS_TRAIT_DEFINITION(args[1])) {
      vm_runtime_error(
          vm, "__builtin_bind: BUILTIN_DESCRIBABLE expects a trait definition");
    }
    vm->builtins.describable = AS_TRAIT_DEFINITION(args[1]);
    break;
  default:
    vm_runtime_error(vm, "__builtin_bind: unknown target id %d", target);
  }

  return NIL_VALUE;
}

static bool implement_describable_for_obj_string(VirtualMachine *vm);

bool bootstrap(VirtualMachine *vm) {
  vm_define_native(vm, "__builtin_bind", native_builtin_bind);

  struct {
    const char *name;
    double value;
  } constants[] = {
      {"__BUILTIN_ITERABLE", (double)BUILTIN_ITERABLE},
      {"__BUILTIN_DESCRIBABLE", (double)BUILTIN_DESCRIBABLE},
  };
  for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++) {
    vm_define_global(vm, constants[i].name, NUMBER_VALUE(constants[i].value));
  }

  const char *sources[] = {PRELUDE_ITERABLE, PRELUDE_DESCRIBABLE};
  int count = (int)(sizeof(sources) / sizeof(sources[0]));

  for (int i = 0; i < count; i++) {
    InterpretResult result = vm_interpret(vm, sources[i]);
    if (result != INTERPRET_OK) {
      fprintf(stderr, "bootstrap: failed to load prelude '%s'\n",
              MANIFEST[i].name);
      return false;
    }
  }

  if (vm->builtins.iterable == NULL) {
    fprintf(stderr, "bootstrap: vm->builtins.iterable was not set — "
                    "did iterable.dt call __builtin_bind?\n");
    return false;
  }
  if (vm->builtins.describable == NULL) {
    fprintf(stderr, "bootstrap: vm->builtins.describable was not set — "
                    "did describable.dt call __builtin_bind?\n");
    return false;
  }

  vm_undefine_global(vm, "__builtin_bind");

  implement_describable_for_obj_string(vm);

  return true;
}

static Value describe_string(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  (void)arg_count;

  Value self = args[-1];
  assert(IS_TRAIT_OBJECT(self) && AS_TRAIT_OBJECT(self)->impl->trait == vm->builtins.describable &&
         "describe_string should only be called as an implementation of Describable for String");

  ObjectString *str = (ObjectString*)AS_TRAIT_OBJECT(self)->receiver;
  char buf[256];
  snprintf(buf, sizeof(buf), "\"%.*s\"", str->length, str->chars);

  char *copy = copy_string(buf, (int)strlen(buf), &vm->al);
  int length = (int)strlen(buf);
  ObjectString *result =
      vm_new_string(vm, copy, length, hash_string(copy, length));
  return OBJECT_VALUE(result);
}

static bool implement_describable_for_obj_string(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {"describe", describe_string},
  };
  return vm_register_native_impl(vm, OBJECT_STRING, vm->builtins.describable,
                                 methods, sizeof(methods) / sizeof(methods[0]));
}