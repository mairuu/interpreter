#include "builtins.h"

#include <complex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "garbage_collector.h"
#include "object.h"
#include "string_utils.h"
#include "virtual_machine.h"

static Value native_clock(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  (void)args;
  if (arg_count != 0) {
    vm_runtime_error(vm, "clock() takes no arguments");
    return NIL_VALUE;
  }
  return NUMBER_VALUE((double)clock() / CLOCKS_PER_SEC);
}

static Value native_print(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  static char buf[1024];
  for (int i = 0; i < arg_count; i++) {
    value_print(buf, sizeof(buf), args[i]);
    printf("%s", buf);
    if (i < arg_count - 1) {
      printf(" ");
    }
  }
  return NIL_VALUE;
}

static Value native_println(VirtualMachine *vm, int arg_count, Value *args) {
  native_print(vm, arg_count, args);
  printf("\n");
  return NIL_VALUE;
}

static Value native_type(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 1) {
    vm_runtime_error(vm, "type() takes exactly one argument");
    return NIL_VALUE;
  }

  switch (args[0].type) {
  case VALUE_NIL:
    return OBJECT_VALUE(vm->builtins.type_nil);
  case VALUE_BOOL:
    return OBJECT_VALUE(vm->builtins.type_bool);
  case VALUE_NUMBER:
    return OBJECT_VALUE(vm->builtins.type_number);
  case VALUE_OBJECT:
    return OBJECT_VALUE(vm->builtins.type_object);
  case VALUE_EMPTY:
    return OBJECT_VALUE(vm->builtins.type_empty);
    break;
  }

  return NIL_VALUE;
}

static Value native_number(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 1) {
    vm_runtime_error(vm, "number() takes exactly one argument");
    return NIL_VALUE;
  }

  Value arg = args[0];
  switch (arg.type) {
  case VALUE_BOOL:
    return NUMBER_VALUE(arg.as.boolean ? 1 : 0);
  case VALUE_NUMBER:
    return arg;
  case VALUE_NIL:
    return NIL_VALUE;
  case VALUE_OBJECT:
    if (IS_STRING(arg)) {
      char *endptr;
      double num = strtod(AS_STRING(arg)->chars, &endptr);
      if (*endptr == '\0') {
        return NUMBER_VALUE(num);
      }
    }
    break;
  case VALUE_EMPTY:
    return NIL_VALUE;
  }

  return NIL_VALUE;
}

// strigify a value
static Value native_str(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  if (arg_count != 1) {
    vm_runtime_error(vm, "str() takes exactly one argument");
    return NIL_VALUE;
  }

  char buf[64];
  buf[0] = '\0';

  Value arg = args[0];
  switch (arg.type) {
  case VALUE_BOOL:
    snprintf(buf, sizeof(buf), "%s", AS_BOOL(arg) ? "true" : "false");
    break;
  case VALUE_NUMBER:
    snprintf(buf, sizeof(buf), "%g", AS_NUMBER(arg));
    break;
  case VALUE_NIL:
    return OBJECT_VALUE(vm->builtins.type_nil);
  case VALUE_OBJECT:
    if (IS_STRING(arg)) {
      return arg;
    }
    break;
  case VALUE_EMPTY:
    break;
  }

  int len = strlen(buf);

  hash_string(buf, len);
  char *copy = copy_string(buf, len, &vm->al);
  ObjectString *string = vm_new_string(vm, copy, len, hash_string(copy, len));

  return OBJECT_VALUE(string);
}

static Value native_panic(VirtualMachine *vm, int arg_count, Value *args) {
  static char buf[1024];
  int offset = 0;
  for (int i = 0; i < arg_count && offset < (int)sizeof(buf) - 1; i++) {
    if (i > 0) {
      offset += snprintf(buf + offset, sizeof(buf) - offset, " ");
    }
    offset += value_print(buf + offset, sizeof(buf) - offset, args[i]);
  }
  vm_runtime_error(vm, "%s", buf);
  return NIL_VALUE;
}

static Value native_readline(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  (void)arg_count;
  (void)args;

  static char buf[1024];

  if (fgets(buf, sizeof(buf), stdin) == NULL) {
    return NIL_VALUE;
  }

  int length = strlen(buf);
  buf[length - 1] = '\0';
  length--;
  char *copy = copy_string(buf, length, &vm->al);
  ObjectString *string =
      vm_new_string(vm, copy, length, hash_string(copy, length));
  return OBJECT_VALUE(string);
}

void builtins_init(BuiltinRegistry *reg, VirtualMachine *vm) {
  *reg = (BuiltinRegistry){0};

  reg->type_bool = vm_intern_string(vm, "bool", 4);
  reg->type_nil = vm_intern_string(vm, "nil", 3);
  reg->type_number = vm_intern_string(vm, "number", 6);
  reg->type_object = vm_intern_string(vm, "object", 6);
  reg->type_empty = vm_intern_string(vm, "empty", 5);
  reg->type_string = vm_intern_string(vm, "string", 6);
}

void builtins_register(BuiltinRegistry *reg, VirtualMachine *vm) {
  (void)reg;
  static const BuiltinDef stdlib[] = {
      {"clock", native_clock},       {"print", native_print},
      {"println", native_println},   {"type", native_type},
      {"number", native_number},     {"str", native_str},
      {"readline", native_readline}, {"panic", native_panic},

  };

  int count = sizeof(stdlib) / sizeof(stdlib[0]);

  for (int i = 0; i < count; i++) {
    vm_define_native(vm, stdlib[i].name, stdlib[i].fn);
  }
}

void builtins_destroy(BuiltinRegistry *reg) {
  reg->type_bool = NULL;
  reg->type_nil = NULL;
  reg->type_number = NULL;
  reg->type_object = NULL;
  reg->type_empty = NULL;
  reg->type_string = NULL;

  reg->iterable = NULL;
}