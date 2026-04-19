#include "virtual_machine.h"

#include "builtins.h"
#include "chunk.h"
#include "compiler.h"
#include "dynamic_array.h"
#include "hash_table.h"
#include "memory.h"
#include "object.h"
#include "opcode.h"
#include "string_utils.h"
#include "value.h"

#include <assert.h>
#include <complex.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <time.h>

#if defined(DEBUG_TRACE_EXECUTION) || defined(DEBUG_PRINT_CODE)
#include "debug.h"

static void print_val(Value value) {
  static char buf[256];
  value_print(buf, sizeof(buf), value);
  printf("%s", buf);
}
#endif

static inline void vm_push(VirtualMachine *vm, Value value);
static inline Value vm_pop(VirtualMachine *vm);
static inline Value vm_peek(VirtualMachine *vm, int distance);

static void vm_reset_stack(VirtualMachine *vm) {
  vm->frame_count = 0;
  vm->stack.top = vm->stack.values;
}

void vm_track_object(VirtualMachine *vm, Object *object) {
  assert(object != NULL && "cannot track NULL object");
  assert(object->next == NULL && "object should not be tracked yet");
  object->next = vm->objects;
  vm->objects = object;
}

void vm_begin_staging(VirtualMachine *vm) { vm->gc_disabled++; }

void vm_end_staging(VirtualMachine *vm) {
  assert(vm->gc_disabled > 0 && "unbalanced staging calls");
  vm->gc_disabled--;
}

ObjectString *vm_new_string(VirtualMachine *vm, char *chars, int length,
                            uint32_t hash) {
  ObjectString *string = obj_string_new(&vm->al, chars, length, hash);
  vm_track_object(vm, &string->object);
  return string;
}

ObjectFunction *vm_new_function(VirtualMachine *vm, int arity) {
  ObjectFunction *function = obj_function_new(&vm->al, arity);
  vm_track_object(vm, &function->object);
  return function;
}

ObjectUpvalue *vm_new_upvalue(VirtualMachine *vm, Value *location) {
  ObjectUpvalue *upvalue = obj_upvalue_new(&vm->al, location);
  vm_track_object(vm, &upvalue->object);
  return upvalue;
}

ObjectClosure *vm_new_closure(VirtualMachine *vm, ObjectFunction *function) {
  ObjectClosure *closure = obj_closure_new(&vm->al, function);
  vm_track_object(vm, &closure->object);
  return closure;
}

ObjectNative *vm_new_native(VirtualMachine *vm, NativeFunc function) {
  ObjectNative *native = obj_native_new(&vm->al, function);
  vm_track_object(vm, &native->object);
  return native;
}

ObjectStructDefinition *vm_new_struct_definition(VirtualMachine *vm,
                                                 ObjectString *name,
                                                 uint16_t definition_id) {
  ObjectStructDefinition *def =
      obj_struct_definition_new(&vm->al, name, definition_id);
  vm_track_object(vm, &def->object);
  return def;
}

ObjectStructInstance *vm_new_struct_instance(VirtualMachine *vm,
                                             ObjectStructDefinition *def) {
  ObjectStructInstance *instance = obj_struct_instance_new(&vm->al, def);
  vm_track_object(vm, &instance->obj);
  return instance;
}

ObjectTraitDefinition *vm_new_trait_definition(VirtualMachine *vm,
                                               ObjectString *name,
                                               uint16_t trait_id) {
  ObjectTraitDefinition *trait =
      obj_trait_definition_new(&vm->al, name, trait_id);
  vm_track_object(vm, &trait->object);
  return trait;
}

ObjectImpl *vm_new_impl(VirtualMachine *vm, ObjectTraitDefinition *trait,
                        ObjectStructDefinition *struct_def) {
  ObjectImpl *impl = obj_impl_new(&vm->al, trait, struct_def);
  vm_track_object(vm, &impl->object);
  return impl;
}

ObjectTraitObject *vm_new_trait_object(VirtualMachine *vm,
                                       ObjectStructInstance *instance,
                                       ObjectImpl *impl) {
  ObjectTraitObject *trait_object =
      obj_trait_object_new(&vm->al, instance, impl);
  vm_track_object(vm, &trait_object->object);
  return trait_object;
}

// todo: bound method
// static ObjectBoundMethod *
// vm_new_bound_method(VirtualMachine *vm, Value receiver, ObjectClosure
// *method) {
//   ObjectBoundMethod *bound_method =
//       obj_bound_method_new(&vm->al, receiver, method);
//   vm_track_object(vm, &bound_method->object);
//   return bound_method;
// }

static uint32_t vm_next_definition_id(VirtualMachine *vm) {
  return vm->next_definition_id++;
}

static uint32_t vm_next_trait_id(VirtualMachine *vm) {
  return vm->next_trait_id++;
}

ObjectString *vm_intern_string(VirtualMachine *vm, const char *chars,
                               int length) {
  uint32_t hash = hash_string(chars, length);

  ObjectString temp_str = obj_string_create(chars, length, hash);
  Value *existing = ht_get(&vm->strings, OBJECT_VALUE(&temp_str));

  if (existing != NULL) {
    return AS_STRING(*existing);
  }

  vm_begin_staging(vm);
  char *copy = copy_string(chars, length, &vm->al);
  ObjectString *string = vm_new_string(vm, copy, length, hash);
  string->is_interned = true;
  if (string == NULL) {
    vm_end_staging(vm);
    return NULL;
  }

  ht_put(&vm->strings, OBJECT_VALUE(string), OBJECT_VALUE(string), &vm->al);
  vm_end_staging(vm);
  return string;
}

void vm_define_native(VirtualMachine *vm, const char *name,
                      NativeFunc function) {
  vm_begin_staging(vm);
  Value string = OBJECT_VALUE(vm_intern_string(vm, name, strlen(name)));
  Value native = OBJECT_VALUE(vm_new_native(vm, function));
  ht_put(&vm->globals, string, native, &vm->al);
  vm_end_staging(vm);
}

static ObjectFunction *vm_load_proto(VirtualMachine *vm, Proto *proto);

static Value load_constant(ConstantLoader *loader, Allocator *al,
                           const RawConstant *raw_constant) {
  (void)al;
  VirtualMachine *vm = (VirtualMachine *)loader->ctx;

  switch (raw_constant->type) {
  case RAW_NIL:
    return NIL_VALUE;
  case RAW_BOOL:
    return BOOL_VALUE(raw_constant->as.boolean);
  case RAW_NUMBER:
    return NUMBER_VALUE(raw_constant->as.number);
  case RAW_STRING: {
    ObjectString *interned = vm_intern_string(vm, raw_constant->as.string.chars,
                                              raw_constant->as.string.length);
    if (interned == NULL) {
      assert(false && "out of memory for string constant");
      return NIL_VALUE;
    }
    return OBJECT_VALUE(interned);
  }
  case RAW_FUNC: {
    Proto *proto = raw_constant->as.proto;
    ObjectFunction *function = vm_load_proto(vm, proto);
    return OBJECT_VALUE(function);
  }
  case RAW_STRUCT_DEF:
    const RawStructDef *raw_def = &raw_constant->as.struct_def;

    uint16_t definition_id = vm_next_definition_id(vm);
    ObjectString *name =
        vm_intern_string(vm, raw_def->name.chars, raw_def->name.length);
    ObjectStructDefinition *struct_def =
        vm_new_struct_definition(vm, name, definition_id);

    int field_count = array_count(raw_def->fields);
    for (int i = 0; i < field_count; i++) {
      ObjectString *field_name = vm_intern_string(vm, raw_def->fields[i].chars,
                                                  raw_def->fields[i].length);
      ht_put(&struct_def->fields, OBJECT_VALUE(field_name), NUMBER_VALUE(i),
             &vm->al);
    }
    return OBJECT_VALUE(struct_def);
  case RAW_TRAIT_DEF: {
    const RawTraitDef *raw_def = &raw_constant->as.trait_def;

    uint16_t trait_id = vm_next_trait_id(vm);
    ObjectString *name =
        vm_intern_string(vm, raw_def->name.chars, raw_def->name.length);
    ObjectTraitDefinition *trait_def =
        vm_new_trait_definition(vm, name, trait_id);

    int method_count = array_count(raw_def->methods);
    for (int i = 0; i < method_count; i++) {
      ObjectString *method_name = vm_intern_string(
          vm, raw_def->methods[i].chars, raw_def->methods[i].length);
      array_push(trait_def->method_names, method_name, &vm->al);
    }
    return OBJECT_VALUE(trait_def);
  }
  case RAW_VARIANT_DEF: {
    assert(false && "variant definitions are not supported as constants yet");
    break;
  }
  default:
    assert(false && "invalid constant type");
  }

  return NIL_VALUE;
}

static ObjectFunction *vm_load_proto(VirtualMachine *vm, Proto *proto) {
  vm_begin_staging(vm);

  ObjectFunction *function = vm_new_function(vm, proto->arity);
  ConstantLoader loader = {
      .load_constant = load_constant,
      .ctx = vm,
  };

  chunk_load_from_proto(&function->chunk, proto, &loader, &vm->al);
  function->upvalue_count = proto->upvalue_count;

  if (!str_is_empty(proto->name)) {
    function->name =
        vm_intern_string(vm, proto->name.chars, proto->name.length);
  }

#ifdef DEBUG_PRINT_CODE
  const char *function_name = function->name ? function->name->chars
                              : proto->type == PROTO_SCRIPT ? "<script>"
                                                            : "<anonymous>";
  disassemble_chunk(&function->chunk, function_name);
#endif

  proto_destroy(proto, &vm->al);
  al_free_for(&vm->al, proto);
  vm_end_staging(vm);

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

static void vm_print_stack_trace(VirtualMachine *vm) {
  fprintf(stderr, "stack trace (most recent call last):\n");
  for (int i = vm->frame_count - 1; i >= 0; i--) {
    CallFrame *frame = &vm->frames[i];
    ObjectFunction *fn = frame->function;
    size_t instruction = frame->ip - fn->chunk.instructions - 1;
    int line = fn->chunk.lines[instruction];

    fprintf(stderr, "  [line %d] in ", line);
    if (fn->name == NULL) {
      fprintf(stderr, "<script>\n");
    } else {
      fprintf(stderr, "%s()\n", fn->name->chars);
    }
  }
}

void vm_runtime_error(VirtualMachine *vm, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(vm->panic_message, sizeof(vm->panic_message), format, args);
  va_end(args);

  vm_print_stack_trace(vm);

  longjmp(vm->panic_jump, 1);
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

static ObjectStructInstance *vm_extract_struct_instance(VirtualMachine *vm,
                                                        Value value) {
  if (IS_STRUCT_INSTANCE(value)) {
    return AS_STRUCT_INSTANCE(value);
  }

  if (IS_TRAIT_OBJECT(value)) {
    ObjectTraitObject *trait_object = AS_TRAIT_OBJECT(value);
    // if (trait_object->impl->trait == trait) {
    //   return trait_object->instance; // signal: already the right trait
    // }
    return trait_object->instance;
  }

  vm_runtime_error(vm,
                   "value does not satisfy the constraint: expected a struct "
                   "instance or trait object");
  return NULL;
}

static ObjectTraitObject *vm_cast_trait(VirtualMachine *vm, Value value,
                                        ObjectTraitDefinition *trait) {
  if (IS_TRAIT_OBJECT(value)) {
    ObjectTraitObject *trait_object = AS_TRAIT_OBJECT(value);
    if (trait_object->impl->trait == trait) {
      return trait_object;
    }
  }

  ObjectStructInstance *instance = vm_extract_struct_instance(vm, value);
  if (instance == NULL) {
    return NULL;
  }

  ObjectImpl *impl = obj_struct_definition_find_impl(instance->def, trait);
  if (impl != NULL) {
    return vm_new_trait_object(vm, instance, impl);
  }

  vm_runtime_error(vm, "no implementation found for trait '%s' on struct '%s'",
                   trait->name->chars, instance->def->name->chars);
  return NULL;
}

static bool vm_validate_argument_constraints(VirtualMachine *vm,
                                             ObjectFunction *function,
                                             int arg_count) {
  for (int i = 0; i < arg_count; i++) {
    ObjectTraitDefinition *constraint = function->constraints[i];
    if (!constraint) {
      continue; // no constraint for this parameter
    }

    Value arg = vm_peek(vm, arg_count - 1 - i);
    ObjectTraitObject *trait_object = vm_cast_trait(vm, arg, constraint);
    if (!trait_object) {
      return false; // vm_cast_trait already reports the error
    }

    // replace the argument with the trait object
    vm->stack.top[i - arg_count] = OBJECT_VALUE(trait_object);
  }
  return true;
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

  if (!vm_validate_argument_constraints(vm, function, arg_count)) {
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

static bool vm_call_object(VirtualMachine *vm, Object *callee, int arg_count) {
  switch (callee->type) {
  case OBJECT_FUNCTION: {
    return vm_call(vm, (void *)callee, arg_count);
  }
  case OBJECT_CLOSURE: {
    return vm_call_closure(vm, (void *)callee, arg_count);
  }
  case OBJECT_NATIVE: {
    ObjectNative *native = (ObjectNative *)callee;
    Value result = native->function(vm, arg_count, vm->stack.top - arg_count);
    vm->stack.top -= arg_count + 1;
    vm_push(vm, result);
    return true;
  }
  case OBJECT_STRUCT_DEFINITION:
    return vm_call_struct_constructor(vm, (void *)callee, arg_count);
  default:
    break; // non-callable object type
  }

  vm_runtime_error(vm, "callee is not callable");
  return false;
}

static bool vm_call_value(VirtualMachine *vm, Value callee, int arg_count) {
  if (IS_OBJECT(callee)) {
    return vm_call_object(vm, AS_OBJECT(callee), arg_count);
  }

  vm_runtime_error(vm, "callee is not callable");
  return false;
}

void vm_init(VirtualMachine *vm, Allocator al) {
  vm_reset_stack(vm);

  vm->al = al;
  vm->objects = NULL;
  vm->gc_disabled = 0;

  vm->next_definition_id = 1;
  vm->next_trait_id = 1;

  ht_init(&vm->strings, &vm->al);
  ht_init(&vm->globals, &vm->al);

  vm->open_upvalues = NULL;

  builtins_init(&vm->builtins, vm);
  builtins_register(&vm->builtins, vm);
}

void vm_destroy(VirtualMachine *vm) {
  vm_reset_stack(vm);
  builtins_destroy(&vm->builtins);

  ht_destroy(&vm->globals, &vm->al);
  ht_destroy(&vm->strings, &vm->al);

  while (vm->objects) {
    Object *next = vm->objects->next;
    obj_free(&vm->objects, &vm->al);
    vm->objects = next;
  }
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
  if (IS_TRAIT_OBJECT(receiver)) {
    receiver = OBJECT_VALUE(AS_TRAIT_OBJECT(receiver)->instance);
  }

  if (!IS_STRUCT_INSTANCE(receiver)) {
    vm_runtime_error(vm, "only struct instances have fields");
    return NULL;
  }

  ObjectStructInstance *instance = AS_STRUCT_INSTANCE(receiver);

  // 0 is uninitialized cached_id, id starts from 1
  if (cached_id != 0 && instance->def->definition_id == cached_id) {
    return &instance->fields[cached_offset];
  }

  ObjectString *name = AS_STRING(chunk->constants[name_idx]);
  Value *offset_val = ht_get(&instance->def->fields, OBJECT_VALUE(name));

  if (offset_val) {
    uint8_t offset = (uint8_t)AS_NUMBER(*offset_val);
    // patch: [op, name, id(2), slot(1)]
    //         0    1     2            4
    int instruction_start = (ip - 5) - chunk->instructions;
    chunk_patch_short(chunk, instruction_start + 2,
                      instance->def->definition_id);
    chunk_patch_byte(chunk, instruction_start + 4, offset);

    return &instance->fields[offset];
  }

  vm_runtime_error(vm, "undefined field '%s'.", name->chars);
  return NULL;
}

static Object *vm_method_reference(VirtualMachine *vm, Value receiver,
                                   uint8_t *ip, Chunk *chunk, uint8_t name_idx,
                                   uint16_t cached_trait_id,
                                   uint8_t cached_slot) {
  if (!IS_TRAIT_OBJECT(receiver)) {
    vm_runtime_error(vm, "receiver is not a trait object");
    return NULL;
  }

  ObjectTraitObject *fat = AS_TRAIT_OBJECT(receiver);

  // 0 is uninitialized cached_id, id starts from 1
  if (cached_trait_id != 0 && fat->impl->trait->trait_id == cached_trait_id) {
    return fat->impl->methods[cached_slot];
  }

  ObjectString *name = AS_STRING(chunk->constants[name_idx]);

  int slot = obj_trait_find_slot(fat->impl->trait, name);
  if (slot >= UINT8_MAX) {
    vm_runtime_error(vm, "trait method slot overflow");
    return NULL;
  }
  if (slot == -1) {
    vm_runtime_error(vm, "trait '%s' has no method '%s'",
                     fat->impl->trait->name->chars, name->chars);
    return NULL;
  }

  // patch: [op, name, trait_id(2), slot(1), arg_count]
  //         0    1     2            4        5
  int instruction_start = (ip - 6) - chunk->instructions;
  chunk_patch_short(chunk, instruction_start + 2, fat->impl->trait->trait_id);
  chunk_patch_byte(chunk, instruction_start + 4, (uint8_t)slot);

  return fat->impl->methods[slot];
}

static bool vm_bind_constraint(VirtualMachine *vm, Value target,
                               Value constraint, uint8_t param_idx) {
  (void)vm;
  if (!IS_TRAIT_DEFINITION(constraint)) {
    return false; // constraint must be a trait definition
  }

  assert((IS_FUNCTION(target) || IS_CLOSURE(target)) &&
         "target must be a function or closure");

  ObjectTraitDefinition *trait = AS_TRAIT_DEFINITION(constraint);

  ObjectFunction *function =
      IS_FUNCTION(target) ? AS_FUNCTION(target) : AS_CLOSURE(target)->function;

  return obj_function_bind_constraint(function, param_idx, trait);
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
      print_val(*slot);
      printf(" %c", frame->base == slot ? '|' : ']');
    }
    printf("\n");

    disassemble_chunk_instruction(
        &frame->function->chunk,
        (size_t)(ip - frame->function->chunk.instructions));
#endif
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
    case OP_POP_SECOND: {
      vm->stack.top[-2] = vm->stack.top[-1];
      vm->stack.top--;
      break;
    }
    case OP_MATCH: {
      vm->stack.top[-1] =
          BOOL_VALUE(value_equals(vm_peek(vm, 1), vm_peek(vm, 0)));
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
    case OP_GET_PROPERTY: {
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
    case OP_SET_PROPERTY: {
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
    case OP_CONSTRAINT: {
      uint8_t slot_idx = READ_BYTE();

      if (!vm_bind_constraint(vm, vm_peek(vm, 1), (vm_peek(vm, 0)), slot_idx)) {
        vm_runtime_error(vm, "constraint is not bindable");
        break;
      }

      vm_pop(vm); // pop the constraint
      // leaves target
      break;
    };
    case OP_CLOSE_UPVALUE: {
      vm_close_upvalues(vm, vm->stack.top - 1);
      break;
    }

    case OP_CAST_TRAIT: {
      Value trait_val = vm_peek(vm, 0);
      if (!IS_TRAIT_DEFINITION(trait_val)) {
        vm_runtime_error(vm, "expected a trait definition for cast trait");
        break;
      }

      ObjectTraitDefinition *trait = AS_TRAIT_DEFINITION(trait_val);
      ObjectTraitObject *trait_object =
          vm_cast_trait(vm, vm_peek(vm, 1), trait);
      if (!trait_object) {
        break; // vm_cast_trait already reports the error
      }

      vm_pop(vm); // pop the trait definition
      vm_pop(vm); // pop the struct instance
      vm_push(vm, OBJECT_VALUE(trait_object));
      break;
    }
    case OP_CALL_METHOD: {
      uint8_t name_idx = READ_BYTE();
      uint16_t cached_trait = READ_SHORT();
      uint8_t cached_slot = READ_BYTE();
      uint8_t arg_count = READ_BYTE();
      frame->ip = ip;

      Value receiver = vm_peek(vm, arg_count);
      Object *method =
          vm_method_reference(vm, receiver, ip, &frame->function->chunk,
                              name_idx, cached_trait, cached_slot);
      if (method && vm_call_object(vm, method, arg_count)) {
        frame = &vm->frames[vm->frame_count - 1];
        ip = frame->ip;
      }
      break;
    }

    case OP_IMPL: {
      Value trait_val = vm_peek(vm, 1);
      if (!IS_TRAIT_DEFINITION(trait_val)) {
        vm_runtime_error(vm, "expected a trait definition for impl");
        break;
      }
      Value struct_val = vm_peek(vm, 0);
      if (!IS_STRUCT_DEFINITION(struct_val)) {
        vm_runtime_error(vm, "expected a struct definition for impl");
        break;
      }

      ObjectStructDefinition *struct_def = AS_STRUCT_DEFINITION(struct_val);
      ObjectTraitDefinition *trait_def = AS_TRAIT_DEFINITION(trait_val);
      if (obj_struct_definition_find_impl(struct_def, trait_def) != NULL) {
        vm_runtime_error(
            vm, "struct '%s' already has an implementation for trait '%s'",
            struct_def->name->chars, trait_def->name->chars);
        break;
      }

      ObjectImpl *impl = vm_new_impl(vm, trait_def, struct_def);
      vm_pop(vm); // pop the trait definition
      vm_pop(vm); // pop the struct definition
      vm_push(vm, OBJECT_VALUE(impl));
      break;
    }
    case OP_IMPL_METHOD: {
      Value method_val = vm_peek(vm, 0);
      if (IS_FUNCTION(method_val)) {
        // todo: we do not need to normalize anymore
        // normalize method to closure
        ObjectClosure *closure = vm_new_closure(vm, AS_FUNCTION(method_val));
        method_val = OBJECT_VALUE(closure);
        vm_pop(vm);
        vm_push(vm, OBJECT_VALUE(closure));
      }
      assert(IS_CLOSURE(method_val) &&
             "method should be a function or closure");

      ObjectImpl *impl = AS_IMPL(vm_peek(vm, 1));
      ObjectClosure *method = AS_CLOSURE(method_val);

      int slot = obj_trait_find_slot(impl->trait, method->function->name);
      if (slot == -1) {
        vm_runtime_error(vm, "method '%s' not found in trait '%s'",
                         method->function->name->chars,
                         impl->trait->name->chars);
        break;
      }

      impl->methods[slot] = (Object *)method;
      vm_pop(vm); // pop the method
      break;
    }
    case OP_IMPL_COMMIT: {
      Value impl_val = vm_peek(vm, 0);
      assert(IS_IMPL(impl_val) && "expected an impl on the stack");
      ObjectImpl *impl = AS_IMPL(impl_val);

      int method_count = array_count(impl->trait->method_names);
      for (int i = 0; i < method_count; i++) {
        if (impl->methods[i] == NULL) {
          vm_runtime_error(vm,
                           "method '%s' not implemented for struct '%s' "
                           "required by trait '%s'",
                           impl->trait->method_names[i]->chars,
                           impl->struct_def->name->chars,
                           impl->trait->name->chars);
          break;
        }
      }
      array_push(impl->struct_def->impls, impl, &vm->al);
      vm_pop(vm); // pop the impl
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

static InterpretResult vm_execute_script(VirtualMachine *vm,
                                         ObjectFunction *function) {
  vm_push(vm, OBJECT_VALUE(function));
  if (!vm_call(vm, function, 0)) {
    return INTERPRET_RUNTIME_ERROR;
  }

  if (!vm_run(vm)) {
    return INTERPRET_RUNTIME_ERROR;
  }

  return INTERPRET_OK;
}

InterpretResult vm_interpret(VirtualMachine *vm, const char *source) {
  Proto *proto = compile(source, &vm->al);
  if (proto == NULL) {
    return INTERPRET_COMPILE_ERROR;
  }

  if (setjmp(vm->panic_jump)) {
    vm_reset_stack(vm);
    fprintf(stderr, "panic: %s\n", vm->panic_message);
    return INTERPRET_RUNTIME_ERROR;
  }

  ObjectFunction *function = vm_load_proto(vm, proto);
  assert(!vm->gc_disabled &&
         "gc should not be disabled when executing a script");

  return vm_execute_script(vm, function);
}