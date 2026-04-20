#include "bootstrap.h"

#include "virtual_machine.h"

#include <stdio.h>
#include <string.h>

typedef enum {
  BUILTIN_ITERABLE = 0,
  BUILTIN_COUNT,
} BuiltinBindTarget;

static const char *PRELUDE_ITERABLE =
    "trait Iterable { has_next next }\n"
    "__builtin_bind(__BUILTIN_ITERABLE, Iterable)\n";

typedef struct {
  const char *name;
  const char *source;
} PreludeEntry;

static const PreludeEntry MANIFEST[] = {
    {"iterable.dt", NULL},
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

  default:
    vm_runtime_error(vm, "__builtin_bind: unknown target id %d", target);
  }

  return NIL_VALUE;
}

bool bootstrap(VirtualMachine *vm) {
  vm_define_native(vm, "__builtin_bind", native_builtin_bind);

  struct {
    const char *name;
    double value;
  } constants[] = {
      {"__BUILTIN_ITERABLE", (double)BUILTIN_ITERABLE},
  };
  for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++) {
    vm_define_global(vm, constants[i].name, NUMBER_VALUE(constants[i].value));
  }

  const char *sources[] = {PRELUDE_ITERABLE};
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

  vm_undefine_global(vm, "__builtin_bind");

  return true;
}