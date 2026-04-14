#include "virtual_machine.h"
#include "chunk.h"
#include "hash_table.h"
#include "memory.h"
#include "object.h"
#include "opcode.h"
#include "value.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#ifdef DEBUG_TRACE_EXECUTION
#include "debug.h"
#include <stdio.h>
#endif

static void vm_reset(VirtualMachine *vm) { vm->stack.top = vm->stack.values; }

void vm_init(VirtualMachine *vm, Allocator al) {
  vm->al = al;
  vm->objects = NULL;

  chunk_init(&vm->chunk);
  ht_init(&vm->strings, &vm->al);
  ht_init(&vm->globals, &vm->al);

  vm_reset(vm);
}

void vm_destroy(VirtualMachine *vm) {
  vm_reset(vm);

  ht_destroy(&vm->globals, &vm->al);
  ht_destroy(&vm->strings, &vm->al);
  chunk_destroy(&vm->chunk, &vm->al);

  while (vm->objects) {
    Object *next = vm->objects->next;
    object_free(&vm->objects, &vm->al);
    vm->objects = next;
  }
}

static void vm_track_object(VirtualMachine *vm, Object *object) {
  assert(object != NULL);
  object->next = vm->objects;
  vm->objects = object;
}

static ObjectString *vm_new_string(VirtualMachine *vm, char *chars, int length,
                                   uint32_t hash) {
  ObjectString *string = object_string_new(&vm->al, chars, length, hash);
  vm_track_object(vm, &string->object);
  return string;
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

static ObjectString *vm_intern_string(VirtualMachine *vm, char *chars,
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

  ht_put(&vm->strings, OBJECT_VALUE(string), OBJECT_VALUE(string), &vm->al);

  return string;
}

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
  }

  return NIL_VALUE;
}

static void vm_load_proto(VirtualMachine *vm, const Proto *proto) {
  vm_reset(vm);

  ConstantLoader loader = {
      .load_constant = load_constant,
      .ctx = vm,
  };
  chunk_load_from_proto(&vm->chunk, proto, &loader, &vm->al);

#ifdef DEBUG_TRACE_EXECUTION
  disassemble_chunk(&vm->chunk, proto->name);
#endif
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
  (void)vm;
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);
}

static void vm_concatenate(VirtualMachine *vm) {
  ObjectString *b = AS_STRING(vm_pop(vm));
  ObjectString *a = AS_STRING(vm_pop(vm));

  int length = a->length + b->length;
  char *chars = al_alloc(&vm->al, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[a->length + b->length] = '\0';

  uint32_t hash = hash_string(chars, length);
  ObjectString *str = vm_new_string(vm, chars, length, hash);

  vm_push(vm, OBJECT_VALUE(str));
}

static bool vm_run(VirtualMachine *vm) {

  uint8_t *ip = vm->chunk.instructions;
#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (uint16_t)(ip[-2] << 8 | ip[-1]))
#define READ_CONSTANT() (vm->chunk.constants[READ_BYTE()])
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
      printf(" ]");
    }
    printf("\n");

    disassemble_chunk_instruction(&vm->chunk,
                                  (size_t)(ip - vm->chunk.instructions));
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
        return false;
      }
      vm->stack.top[-1].as.number *= -1;
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
        return false;
      }
      Value value = vm_peek(vm, 0);
      ht_put(&vm->globals, OBJECT_VALUE(name), value, &vm->al);
      break;
    }
    case OP_GET_GLOBAL: {
      ObjectString *name = READ_STRING();
      Value *value = ht_get(&vm->globals, OBJECT_VALUE(name));
      if (value == NULL) {
        vm_runtime_error(vm, "undefined variable '%s'", name->chars);
        return false;
      }
      vm_push(vm, *value);
      break;
    }
    case OP_GET_LOCAL: {
      uint8_t slot = READ_BYTE();
      vm_push(vm, vm->stack.values[slot]);
      break;
    }
    case OP_SET_LOCAL: {
      uint8_t slot = READ_BYTE();
      vm->stack.values[slot] = vm_peek(vm, 0);
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

    case OP_PRINT:
      value_print(vm_pop(vm));
      printf("\n");
      break;
    case OP_RETURN:
      return true;

    default:
      assert(false && "unknown opcode");
      vm_runtime_error(vm, "unknown opcode %d", instruction);
      return false;
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

  vm_load_proto(vm, proto);
  proto_destroy(proto, &vm->al);
  al_free_for(&vm->al, proto);

  if (!vm_run(vm)) {
    result = INTERPRET_RUNTIME_ERROR;
  };

  // temp
  chunk_destroy(&vm->chunk, &vm->al);
_exit:
  return result;
}