#include "virtual_machine.h"

#include "chunk.h"
#include "compiler.h"
#include "dynamic_array.h"
#include "hash_table.h"
#include "memory.h"
#include "object.h"
#include "opcode.h"
#include "value.h"

#include <assert.h>
#include <complex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

static inline void vm_push(VirtualMachine *vm, Value value);
static inline Value vm_pop(VirtualMachine *vm);
static inline Value vm_peek(VirtualMachine *vm, int distance);

static void vm_reset_stack(VirtualMachine *vm) {
  vm->frame_count = 0;
  vm->stack.top = vm->stack.values;
}

static void vm_track_object(VirtualMachine *vm, Object *object) {
  assert(object != NULL && "cannot track NULL object");
  assert(object->next == NULL && "object should not be tracked yet");
  object->next = vm->objects;
  vm->objects = object;
}

static ObjectString *vm_new_string(VirtualMachine *vm, char *chars, int length,
                                   uint32_t hash) {
  ObjectString *string = object_string_new(&vm->al, chars, length, hash);
  vm_track_object(vm, &string->object);
  return string;
}

static ObjectFunction *vm_new_function(VirtualMachine *vm) {
  ObjectFunction *function = object_function_new(&vm->al);
  vm_track_object(vm, &function->object);
  return function;
}

static ObjectUpvalue *vm_new_upvalue(VirtualMachine *vm, Value *location) {
  ObjectUpvalue *upvalue = object_upvalue_new(&vm->al, location);
  vm_track_object(vm, &upvalue->object);
  return upvalue;
}

static ObjectClosure *vm_new_closure(VirtualMachine *vm,
                                     ObjectFunction *function) {
  ObjectClosure *closure = object_closure_new(&vm->al, function);
  vm_track_object(vm, &closure->object);
  return closure;
}

static ObjectNative *vm_new_native(VirtualMachine *vm, NavtiveFunc function) {
  ObjectNative *native = object_native_new(&vm->al, function);
  vm_track_object(vm, &native->object);
  return native;
}

static ObjectStructDefinition *
vm_new_struct_definition(VirtualMachine *vm, ObjectString *name,
                         uint16_t definition_id) {
  ObjectStructDefinition *def =
      object_struct_definition_new(&vm->al, name, definition_id);
  vm_track_object(vm, &def->object);
  return def;
}

static ObjectStructInstance *
vm_new_struct_instance(VirtualMachine *vm, ObjectStructDefinition *def) {
  ObjectStructInstance *instance = object_struct_instance_new(&vm->al, def);
  vm_track_object(vm, &instance->obj);
  return instance;
}

static ObjectTraitDefinition *vm_new_trait_definition(VirtualMachine *vm,
                                                      ObjectString *name,
                                                      uint16_t trait_id) {
  ObjectTraitDefinition *trait =
      object_trait_definition_new(&vm->al, name, trait_id);
  vm_track_object(vm, &trait->object);
  return trait;
}

static uint32_t hash_string(const char *str, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)str[i];
    hash *= 16777619u;
  }
  return hash;
}

static char *copy_string(const char *str, int length, Allocator *al) {
  char *copy = al_alloc(al, length + 1);
  memcpy(copy, str, length);
  copy[length] = '\0';
  return copy;
}

static ObjectString *vm_intern_string(VirtualMachine *vm, const char *chars,
                                      int length) {
  uint32_t hash = hash_string(chars, length);

  ObjectString temp_str = object_string_create(chars, length, hash);
  Value *existing = ht_get(&vm->strings, OBJECT_VALUE(&temp_str));
  if (existing != NULL) {
    return AS_STRING(*existing);
  }

  char *copy = copy_string(chars, length, &vm->al);
  ObjectString *string = vm_new_string(vm, copy, length, hash);
  string->is_interned = true;
  if (string == NULL) {
    return NULL;
  }

  Value interned_value = OBJECT_VALUE(string);
  vm_push(vm, interned_value);
  ht_put(&vm->strings, interned_value, interned_value, &vm->al);
  vm_pop(vm);

  return string;
}

static void vm_define_native(VirtualMachine *vm, const char *name,
                             NavtiveFunc function) {
  Value string = OBJECT_VALUE(vm_intern_string(vm, name, strlen(name)));
  vm_push(vm, string);
  Value native = OBJECT_VALUE(vm_new_native(vm, function));
  vm_push(vm, native);
  ht_put(&vm->globals, string, native, &vm->al);
  vm_pop(vm);
  vm_pop(vm);
}

static ObjectFunction *vm_load_proto(VirtualMachine *vm, Proto *proto);

static Value load_constant(ConstantLoader *loader, Allocator *al,
                           const RawConstant *raw_constant) {
  (void)al;

  switch (raw_constant->type) {
  case RAW_NIL:
    return NIL_VALUE;
  case RAW_BOOL:
    return BOOL_VALUE(raw_constant->as.boolean);
  case RAW_NUMBER:
    return NUMBER_VALUE(raw_constant->as.number);
  case RAW_STRING: {
    ObjectString *interned =
        vm_intern_string(loader->ctx, raw_constant->as.string.chars,
                         raw_constant->as.string.length);
    if (interned == NULL) {
      assert(false && "out of memory for string constant");
      return NIL_VALUE;
    }
    return OBJECT_VALUE(interned);
  }
  case RAW_FUNC: {
    Proto *proto = raw_constant->as.proto;
    ObjectFunction *function = vm_load_proto(loader->ctx, proto);
    return OBJECT_VALUE(function);
  }
  }

  return NIL_VALUE;
}

static ObjectFunction *vm_load_proto(VirtualMachine *vm, Proto *proto) {
  ObjectFunction *function = vm_new_function(vm);
  vm_push(vm, OBJECT_VALUE(function));
  ConstantLoader loader = {
      .load_constant = load_constant,
      .ctx = vm,
  };

  chunk_load_from_proto(&function->chunk, proto, &loader, &vm->al);
  function->arity = proto->arity;
  function->upvalue_count = proto->upvalue_count;
  if (proto->name != NULL) {
    function->name = vm_intern_string(vm, proto->name, strlen(proto->name));
  }

#ifdef DEBUG_PRINT_CODE
  const char *function_name = function->name ? function->name->chars
                              : proto->type == PROTO_SCRIPT ? "<script>"
                                                            : "<anonymous>";
  disassemble_chunk(&function->chunk, function_name);
#endif

  proto_destroy(proto, &vm->al);
  al_free_for(&vm->al, proto);
  vm_pop(vm);

  return function;
}

static inline void vm_push(VirtualMachine *vm, Value value) {
  assert(vm->stack.top - vm->stack.values < STACK_MAX);
  *vm->stack.top++ = value;
}

static inline Value vm_pop(VirtualMachine *vm) {
  assert(vm->stack.top > vm->stack.values);
  return *--vm->stack.top;
}

static inline Value vm_peek(VirtualMachine *vm, int distance) {
  assert(vm->stack.top - vm->stack.values > distance);
  return vm->stack.top[-1 - distance];
}

static void vm_runtime_error(VirtualMachine *vm, const char *format, ...) {
  // todo: raise a panic with helper
  vm->in_panic = true;

  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  // print a stack trace.
  for (int i = vm->frame_count - 1; i >= 0; i--) {
    CallFrame *frame = &vm->frames[i];
    ObjectFunction *function = frame->function;
    size_t instruction = frame->ip - function->chunk.instructions - 1;
    fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }
}

static void vm_concatenate(VirtualMachine *vm) {
  ObjectString *b = AS_STRING(vm_peek(vm, 0));
  ObjectString *a = AS_STRING(vm_peek(vm, 1));

  int length = a->length + b->length;
  char *chars = al_alloc(&vm->al, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[a->length + b->length] = '\0';

  uint32_t hash = hash_string(chars, length);
  ObjectString *str = vm_new_string(vm, chars, length, hash);

  vm_pop(vm); // pop b
  vm_pop(vm); // pop a
  vm_push(vm, OBJECT_VALUE(str));
}

static bool vm_call(VirtualMachine *vm, ObjectFunction *function,
                    int arg_count) {
  if (arg_count != function->arity) {
    vm_runtime_error(vm, "expected %d arguments but got %d", function->arity,
                     arg_count);
    return false;
  }
  if (vm->frame_count >= FRAMES_MAX) {
    vm_runtime_error(vm, "stack overflow");
    return false;
  }

  CallFrame *frame = &vm->frames[vm->frame_count++];
  frame->function = function;
  frame->ip = function->chunk.instructions;
  frame->base = vm->stack.top - arg_count - 1;
  return true;
}

static bool vm_call_closure(VirtualMachine *vm, ObjectClosure *closure,
                            int arg_count) {
  if (!vm_call(vm, closure->function, arg_count)) {
    return false;
  };

  vm->frames[vm->frame_count - 1].upvalues = closure->upvalues;
  return true;
}

static bool vm_call_struct_constructor(VirtualMachine *vm,
                                       ObjectStructDefinition *def,
                                       int arg_count) {
  if (arg_count != def->fields.count) {
    vm_runtime_error(vm, "expected %d arguments but got %d", def->fields.count,
                     arg_count);
    return false;
  }

  ObjectStructInstance *instance = vm_new_struct_instance(vm, def);
  for (int i = def->fields.count - 1; i >= 0; i--) {
    instance->fields[i] = vm_pop(vm);
  }

  vm_pop(vm); // pop the struct definition
  vm_push(vm, OBJECT_VALUE(instance));

  return true;
}

static bool vm_call_value(VirtualMachine *vm, Value callee, int arg_count) {
  if (IS_OBJECT(callee)) {
    switch (AS_OBJECT(callee)->type) {
    case OBJECT_FUNCTION: {
      return vm_call(vm, AS_FUNCTION(callee), arg_count);
    }
    case OBJECT_CLOSURE: {
      return vm_call_closure(vm, AS_CLOSURE(callee), arg_count);
    }
    case OBJECT_NATIVE: {
      ObjectNative *native = AS_NATIVE(callee);
      Value result = native->function(vm, arg_count, vm->stack.top - arg_count);
      vm->stack.top -= arg_count + 1;
      vm_push(vm, result);
      return true;
    }
    case OBJECT_STRUCT_DEFINITION:
      return vm_call_struct_constructor(vm, AS_STRUCT_DEFINITION(callee),
                                        arg_count);
    default:
      break; // non-callable object type
    }
  }

  vm_runtime_error(vm, "callee is not callable");
  return false;
}

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
  for (int i = 0; i < arg_count; i++) {
    value_print(args[i]);
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
    return OBJECT_VALUE(vm->type_nil);
  case VALUE_BOOL:
    return OBJECT_VALUE(vm->type_bool);
  case VALUE_NUMBER:
    return OBJECT_VALUE(vm->type_number);
  case VALUE_OBJECT:
    return OBJECT_VALUE(vm->type_object);
  case VALUE_EMPTY:
    return OBJECT_VALUE(vm->type_empty);
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

static Value native_readline(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  (void)arg_count;
  (void)args;

  char buf[1024];

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

void vm_init(VirtualMachine *vm, Allocator al) {
  vm_reset_stack(vm);

  vm->in_panic = false;
  vm->al = al;
  vm->objects = NULL;

  ht_init(&vm->strings, &vm->al);
  ht_init(&vm->globals, &vm->al);

  vm->open_upvalues = NULL;

  vm->type_bool = vm_intern_string(vm, "bool", 4);
  vm->type_nil = vm_intern_string(vm, "nil", 3);
  vm->type_number = vm_intern_string(vm, "number", 6);
  vm->type_object = vm_intern_string(vm, "object", 6);
  vm->type_empty = vm_intern_string(vm, "empty", 5);

  vm_define_native(vm, "clock", native_clock);
  vm_define_native(vm, "print", native_print);
  vm_define_native(vm, "println", native_println);
  vm_define_native(vm, "type", native_type);
  vm_define_native(vm, "number", native_number);
  vm_define_native(vm, "readline", native_readline);
}

void vm_destroy(VirtualMachine *vm) {
  vm_reset_stack(vm);

  ht_destroy(&vm->globals, &vm->al);
  ht_destroy(&vm->strings, &vm->al);

  while (vm->objects) {
    Object *next = vm->objects->next;
    object_free(&vm->objects, &vm->al);
    vm->objects = next;
  }
}

// recover from panic if needed, return true if recovered, false otherwise
static bool vm_recover_from_panic(VirtualMachine *vm) {
  if (!vm->in_panic) {
    return false;
  }
  vm->in_panic = false;
  vm_reset_stack(vm);
  return true;
}

// create or reuse an upvalue for the given local variable
static ObjectUpvalue *vm_capture_upvalue(VirtualMachine *vm, Value *local) {
  ObjectUpvalue *prev_upvalue = NULL;
  ObjectUpvalue *upvalue = vm->open_upvalues;

  while (upvalue != NULL && upvalue->location > local) {
    prev_upvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjectUpvalue *new_upvalue = vm_new_upvalue(vm, local);
  new_upvalue->next = upvalue;

  if (prev_upvalue == NULL) {
    vm->open_upvalues = new_upvalue;
  } else {
    prev_upvalue->next = new_upvalue;
  }

  return new_upvalue;
}

static void vm_close_upvalues(VirtualMachine *vm, Value *last) {
  // close all open upvalues that capture a local variable at or above `last`.
  while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
    ObjectUpvalue *upvalue = vm->open_upvalues;
    upvalue->closed =
        *upvalue->location; // current location is on the stack, so copy the
                            // value to the upvalue object.
    upvalue->location =
        &upvalue->closed; // point the upvalue to the closed value. for any
                          // future access to this upvalue, it will read from
                          // the closed value instead of the stack.
    vm->open_upvalues = upvalue->next;
  }
}

static Value *vm_field_reference(VirtualMachine *vm, Value receiver,
                                 uint8_t *ip, Chunk *chunk, uint8_t name_idx,
                                 uint16_t cached_id, uint8_t cached_offset) {
  if (!IS_STRUCT_INSTANCE(receiver)) {
    vm_runtime_error(vm, "only struct instances have fields");
    return NULL;
  }

  ObjectStructInstance *instance = AS_STRUCT_INSTANCE(receiver);

  if (instance->def->definition_id == cached_id) {
    return &instance->fields[cached_offset];
  }

  ObjectString *name = AS_STRING(chunk->constants[name_idx]);
  Value *offset_val = ht_get(&instance->def->fields, OBJECT_VALUE(name));

  if (offset_val) {
    uint8_t offset = (uint8_t)AS_NUMBER(*offset_val);
    int instruction_start = (ip - 5) - chunk->instructions;

    chunk_patch_short(chunk, instruction_start + 2,
                      instance->def->definition_id);

    chunk_patch_byte(chunk, instruction_start + 4, offset);

    return &instance->fields[offset];
  }

  vm_runtime_error(vm, "undefined field '%s'.", name->chars);
  return NULL;
}

static bool vm_run(VirtualMachine *vm) {
  CallFrame *frame = &vm->frames[vm->frame_count - 1];
  uint8_t *ip = frame->ip;

#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (uint16_t)(ip[-2] << 8 | ip[-1]))
#define READ_CONSTANT() (frame->function->chunk.constants[READ_BYTE()])
#define READ_STRING() (AS_STRING(READ_CONSTANT()))
#define BINARY_OP(val_type, op)                                                \
  do {                                                                         \
    if (!IS_NUMBER(vm_peek(vm, 0)) || !IS_NUMBER(vm_peek(vm, 1))) {            \
      vm_runtime_error(vm, "operands must be numbers");                        \
      return false;                                                            \
    }                                                                          \
    double b = AS_NUMBER(vm_pop(vm));                                          \
    double a = AS_NUMBER(vm_pop(vm));                                          \
    vm_push(vm, val_type(a op b));                                             \
  } while (false)

  while (true) {
#ifdef DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value *slot = vm->stack.values; slot < vm->stack.top; slot++) {
      printf("[ ");
      value_print(*slot);
      printf(" %c", frame->base == slot ? '=' : ']');
    }
    printf("\n");

    disassemble_chunk_instruction(
        &frame->function->chunk,
        (size_t)(ip - frame->function->chunk.instructions));
#endif
    if (vm_recover_from_panic(vm)) {
      return false;
    }

    uint8_t instruction = READ_BYTE();

    switch (instruction) {
    case OP_CONSTANT: {
      Value constant = READ_CONSTANT();
      vm_push(vm, constant);
      break;
    }

    case OP_ADD:
      if (IS_NUMBER(vm_peek(vm, 0)) && IS_NUMBER(vm_peek(vm, 1))) {
        double b = AS_NUMBER(vm_pop(vm));
        double a = AS_NUMBER(vm_pop(vm));
        vm_push(vm, NUMBER_VALUE(a + b));
        break;
      }
      if (IS_STRING(vm_peek(vm, 0)) && IS_STRING(vm_peek(vm, 1))) {
        vm_concatenate(vm);
        break;
      }
      vm_runtime_error(vm, "operands must be two numbers or two strings");
      return false;
    case OP_SUBTRACT:
      BINARY_OP(NUMBER_VALUE, -);
      break;
    case OP_MULTIPLY:
      BINARY_OP(NUMBER_VALUE, *);
      break;
    case OP_DIVIDE:
      BINARY_OP(NUMBER_VALUE, /);
      break;
    case OP_NEGATE:
      if (!IS_NUMBER(vm_peek(vm, 0))) {
        vm_runtime_error(vm, "expected number");
      } else {
        vm->stack.top[-1].as.number *= -1;
      }
      break;

    case OP_EQUAL: {
      Value b = vm_pop(vm);
      Value a = vm_pop(vm);
      vm_push(vm, BOOL_VALUE(value_equals(a, b)));
      break;
    }
    case OP_GREATER:
      BINARY_OP(BOOL_VALUE, >);
      break;
    case OP_LESS:
      BINARY_OP(BOOL_VALUE, <);
      break;
    case OP_NOT:
      vm->stack.top[-1] = BOOL_VALUE(value_is_falsey(vm_peek(vm, 0)));
      break;

    case OP_NIL:
      vm_push(vm, NIL_VALUE);
      break;
    case OP_TRUE:
      vm_push(vm, BOOL_VALUE(true));
      break;
    case OP_FALSE:
      vm_push(vm, BOOL_VALUE(false));
      break;

    case OP_POP: {
      vm->stack.top--;
      break;
    }
    case OP_DEFINE_GLOBAL: {
      ObjectString *name = READ_STRING();
      Value value = vm_peek(vm, 0);
      ht_put(&vm->globals, OBJECT_VALUE(name), value, &vm->al);
      vm_pop(vm);
      break;
    }
    case OP_SET_GLOBAL: {
      ObjectString *name = READ_STRING();
      if (!ht_get(&vm->globals, OBJECT_VALUE(name))) {
        vm_runtime_error(vm, "undefined variable '%s'", name->chars);
      } else {
        Value value = vm_peek(vm, 0);
        ht_put(&vm->globals, OBJECT_VALUE(name), value, &vm->al);
      }
      break;
    }
    case OP_GET_GLOBAL: {
      ObjectString *name = READ_STRING();
      Value *value = ht_get(&vm->globals, OBJECT_VALUE(name));
      if (value == NULL) {
        vm_runtime_error(vm, "undefined variable '%s'", name->chars);
      } else {
        vm_push(vm, *value);
      }
      break;
    }
    case OP_GET_LOCAL: {
      uint8_t slot = READ_BYTE();
      vm_push(vm, frame->base[slot]);
      break;
    }
    case OP_SET_LOCAL: {
      uint8_t slot = READ_BYTE();
      frame->base[slot] = vm_peek(vm, 0);
      break;
    }
    case OP_GET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      vm_push(vm, *frame->upvalues[slot]->location);
      break;
    }
    case OP_SET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      *frame->upvalues[slot]->location = vm_peek(vm, 0);
      break;
    }
    case OP_GET_FIELD: {
      uint8_t name_idx = READ_BYTE();
      uint16_t cached_id = READ_SHORT();
      uint8_t cached_offset = READ_BYTE();

      Value receiver = vm_peek(vm, 0);
      Value *field_ref =
          vm_field_reference(vm, receiver, ip, &frame->function->chunk,
                             name_idx, cached_id, cached_offset);
      if (field_ref) {
        vm_pop(vm); // pop the receiver
        vm_push(vm, *field_ref);
      }
      break;
    }
    case OP_SET_FIELD: {
      uint8_t name_idx = READ_BYTE();
      uint16_t cached_id = READ_SHORT();
      uint8_t cached_offset = READ_BYTE();

      Value receiver = vm_peek(vm, 1);

      Value *field_ref =
          vm_field_reference(vm, receiver, ip, &frame->function->chunk,
                             name_idx, cached_id, cached_offset);
      if (field_ref) {
        *field_ref = vm_peek(vm, 0);
        vm_pop(vm); // pop the value
        vm_pop(vm); // pop the receiver
        vm_push(vm, *field_ref);
      }
      break;
    }

    case OP_JUMP_IF_FALSE: {
      uint16_t offset = READ_SHORT();
      if (value_is_falsey(vm_peek(vm, 0))) {
        ip += offset;
      }
      break;
    }
    case OP_JUMP: {
      uint16_t offset = READ_SHORT();
      ip += offset;
      break;
    }
    case OP_LOOP: {
      uint16_t offset = READ_SHORT();
      ip -= offset;
      break;
    }

    case OP_CLOSURE: {
      ObjectClosure *closure = vm_new_closure(vm, AS_FUNCTION(READ_CONSTANT()));
      vm_push(vm, OBJECT_VALUE(closure));

      int upvalue_count = closure->function->upvalue_count;
      for (int i = 0; i < upvalue_count; i++) {
        uint8_t is_local = READ_BYTE();
        uint8_t index = READ_BYTE();
        if (is_local) {
          closure->upvalues[i] = vm_capture_upvalue(vm, frame->base + index);
        } else {
          closure->upvalues[i] = frame->upvalues[index];
        }
      }
      break;
    }
    case OP_CLOSE_UPVALUE: {
      vm_close_upvalues(vm, vm->stack.top - 1);
      break;
    }

    case OP_STRUCT: {
      // quick hack
      static uint16_t next_definition_id = 1;

      if (next_definition_id == UINT16_MAX) {
        vm_runtime_error(vm, "too many struct definitions (max %d)",
                         UINT16_MAX);
        // this thing is not recoverable
        exit(1);
        break;
      }

      ObjectString *name = READ_STRING();
      ObjectStructDefinition *def =
          vm_new_struct_definition(vm, name, next_definition_id++);

      vm_push(vm, OBJECT_VALUE(def));
      break;
    }
    case OP_STRUCT_FIELD: {
      ObjectString *field_name = READ_STRING();
      ObjectStructDefinition *def = AS_STRUCT_DEFINITION(vm_peek(vm, 0));
      ht_put(&def->fields, OBJECT_VALUE(field_name),
             NUMBER_VALUE(def->fields.count), &vm->al);
      break;
    }

    case OP_TRAIT: {
      static uint16_t next_trait_id = 1;
      ObjectString *name = READ_STRING();
      ObjectTraitDefinition *trait =
          vm_new_trait_definition(vm, name, next_trait_id++);
      vm_push(vm, OBJECT_VALUE(trait));
      break;
    }
    case OP_TRAIT_METHOD: {
      ObjectString *method_name = READ_STRING();
      ObjectTraitDefinition *trait = AS_TRAIT_DEFINITION(vm_peek(vm, 0));
      array_push(trait->method_names, method_name, &vm->al);
      break;
    }

    case OP_CALL: {
      uint8_t arg_count = READ_BYTE();
      frame->ip = ip;
      Value callee = vm_peek(vm, arg_count);
      if (vm_call_value(vm, callee, arg_count)) {
        frame = &vm->frames[vm->frame_count - 1];
        ip = frame->ip;
      }
      break;
    }
    case OP_RETURN: {
      Value result = vm_pop(vm);
      vm_close_upvalues(vm, frame->base);
      vm->frame_count--;
      if (vm->frame_count == 0) {
        vm_pop(vm);
        return true;
      }
      vm->stack.top = frame->base;
      vm_push(vm, result);
      frame = &vm->frames[vm->frame_count - 1];
      ip = frame->ip;
      break;
    }

    default:
      assert(false && "unknown opcode");
      vm_runtime_error(vm, "unknown opcode %d", instruction);
    }
  }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_STRING
#undef READ_CONSTANT
#undef BINARY_OP

  return false;
}

InterpretResult vm_interpret(VirtualMachine *vm, const char *source) {
  InterpretResult result = INTERPRET_OK;

  Proto *proto = compile(source, &vm->al);
  if (proto == NULL) {
    result = INTERPRET_COMPILE_ERROR;
    goto _exit;
  }

  vm_reset_stack(vm);

  // setup call frame for the top-level script
  ObjectFunction *function = vm_load_proto(vm, proto);
  vm_push(vm, OBJECT_VALUE(function));
  vm_call(vm, function, 0);

  if (!vm_run(vm)) {
    result = INTERPRET_RUNTIME_ERROR;
  };

_exit:
  return result;
}