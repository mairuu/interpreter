#include "garbage_collector.h"

#include <assert.h>

#include "builtins.h"
#include "dynamic_array.h"
#include "hash_table.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include "virtual_machine.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>

static void print_obj(Object *obj) {
  static char buf[256];
  obj_print(buf, sizeof(buf), obj);
  printf("%s", buf);
}
#endif

#define GC_INITIAL_THRESHOLD (1024 * 1024) // 1MB
// #define GC_INITIAL_THRESHOLD (512) // debug

static void gc_mark_value(GarbageCollector *gc, Value value);

static void gc_mark_hash_table(GarbageCollector *gc, HashTable *table) {
  HashTableIterator it;
  hti_init(&it, table);
  while (hti_next(&it)) {
    gc_mark_value(gc, *it.key);
    gc_mark_value(gc, *it.value);
  }
}

// remove weekly reachable objects from the hash table
static void ht_remove_unmarked(HashTable *table) {
  HashTableIterator it;
  hti_init(&it, table);
  while (hti_next(&it)) {
    if (IS_OBJECT(*it.key) && !AS_OBJECT(*it.key)->is_marked) {
      ht_delete(table, *it.key);
    }
  }
}

void gc_mark_object(GarbageCollector *gc, Object *obj) {
  if (obj == NULL || obj->is_marked) {
    return;
  }

#ifdef DEBUG_LOG_GC
  printf("%p mark ", (void *)obj);
  print_obj(obj);
  printf("\n");
#endif

  obj->is_marked = true;

  array_push(gc->gray_stack, obj, gc->inner);
}

static void gc_mark_value(GarbageCollector *gc, Value value) {
  if (IS_OBJECT(value)) {
    gc_mark_object(gc, AS_OBJECT(value));
  }
}

static void gc_mark_values(GarbageCollector *gc, Value *values, int count) {
  for (int i = 0; i < count; i++) {
    gc_mark_value(gc, values[i]);
  }
}

static void builtins_gc_visit(BuiltinRegistry *reg, GarbageCollector *gc) {
  const ObjectString *type_names[] = {reg->type_nil,    reg->type_bool,
                                      reg->type_number, reg->type_object,
                                      reg->type_empty,  reg->type_string};
  for (size_t i = 0; i < sizeof(type_names) / sizeof(type_names[0]); i++) {
    gc_mark_object(gc, (Object *)type_names[i]);
  }

  gc_mark_object(gc, (Object *)reg->iterable);
  gc_mark_object(gc, (Object *)reg->into_iterable);
  gc_mark_object(gc, (Object *)reg->result);
  gc_mark_object(gc, (Object *)reg->array);
}

static void vm_gc_visit(GarbageCollector *gc) {
  for (Value *slot = gc->vm->stack.values; slot < gc->vm->stack.top; slot++) {
    gc_mark_value(gc, *slot);
  }

  for (ObjectUpvalue *upvalue = gc->vm->open_upvalues; upvalue != NULL;
       upvalue = upvalue->next) {
    gc_mark_object(gc, (Object *)upvalue);
  }

  gc_mark_hash_table(gc, &gc->vm->globals);

  for (int i = 0; i < OBJECT_TYPE_COUNT; i++) {
    NativeImplEntry *entry = gc->vm->native_impls[i];
    if (entry == NULL) {
      continue;
    }
    int impl_count = array_count(entry);
    for (int j = 0; j < impl_count; j++) {
      gc_mark_object(gc, (Object *)entry[j].trait);
      gc_mark_object(gc, (Object *)entry[j].impl);
    }
  }

  int def_count = array_count(gc->vm->definitions);
  for (int i = 0; i < def_count; i++) {
    gc_mark_object(gc, gc->vm->definition_objects[i]);
  }
}

static void gc_mark_roots(GarbageCollector *gc) {
  vm_gc_visit(gc);
  builtins_gc_visit(&gc->vm->builtins, gc);
}

static void gc_blacken_object(GarbageCollector *gc, Object *obj) {
#ifdef DEBUG_LOG_GC
  printf("%p blacken ", (void *)obj);
  print_obj(obj);
  printf("\n");
#endif

  switch (obj->type) {
  case OBJECT_STRING:
    break;
  case OBJECT_FUNCTION: {
    ObjectFunction *function = (ObjectFunction *)obj;
    gc_mark_object(gc, (Object *)function->name);
    gc_mark_values(gc, function->chunk.constants,
                   array_count(function->chunk.constants));
    for (int i = 0; i < function->arity; i++) {
      gc_mark_object(gc, (Object *)function->constraints[i]);
    }
    break;
  }
  case OBJECT_UPVALUE: {
    ObjectUpvalue *upvalue = (ObjectUpvalue *)obj;
    gc_mark_value(gc, upvalue->closed);
    break;
  }
  case OBJECT_CLOSURE: {
    ObjectClosure *closure = (ObjectClosure *)obj;
    gc_mark_object(gc, (Object *)closure->function);
    for (int i = 0; i < closure->upvalue_count; i++) {
      gc_mark_object(gc, (Object *)closure->upvalues[i]);
    }
    break;
  }
  case OBJECT_NATIVE:
    break;
  case OBJECT_STRUCT_DEFINITION: {
    ObjectStructDefinition *def = (ObjectStructDefinition *)obj;
    gc_mark_object(gc, (Object *)def->name);
    gc_mark_hash_table(gc, &def->fields);
    int impl_count = array_count(def->impls);
    for (int i = 0; i < impl_count; i++) {
      gc_mark_object(gc, (Object *)def->impls[i]);
    }
    break;
  }
  case OBJECT_STRUCT_INSTANCE: {
    ObjectStructInstance *instance = (ObjectStructInstance *)obj;
    gc_mark_object(gc, (Object *)instance->def);
    gc_mark_values(gc, instance->fields, instance->def->fields.count);
    break;
  }
  case OBJECT_TRAIT_DEFINITION: {
    ObjectTraitDefinition *trait = (ObjectTraitDefinition *)obj;
    gc_mark_object(gc, (Object *)trait->name);
    for (int i = 0; i < trait->method_count; i++) {
      gc_mark_object(gc, (Object *)trait->method_names[i]);
    }
    break;
  }
  case OBJECT_IMPL: {
    ObjectImpl *impl = (ObjectImpl *)obj;
    gc_mark_object(gc, (Object *)impl->trait);
    gc_mark_object(gc, (Object *)impl->struct_def);
    for (int i = 0; i < impl->trait->method_count; i++) {
      gc_mark_object(gc, (Object *)impl->methods[i]);
    }
    break;
  }
  case OBJECT_TRAIT_OBJECT: {
    ObjectTraitObject *trait_object = (ObjectTraitObject *)obj;
    gc_mark_object(gc, (Object *)trait_object->receiver);
    gc_mark_object(gc, (Object *)trait_object->impl);
    break;
  }
  case OBJECT_BOUND_METHOD: {
    ObjectBoundMethod *bound_method = (ObjectBoundMethod *)obj;
    gc_mark_value(gc, bound_method->receiver);
    gc_mark_object(gc, (Object *)bound_method->method);
    break;
  }
  case OBJECT_VARIANT_DEFINITION: {
    ObjectVariantDefinition *def = (ObjectVariantDefinition *)obj;
    gc_mark_object(gc, &def->name->object);
    for (int i = 0; i < def->arm_count; i++) {
      gc_mark_object(gc, &def->arms[i].name->object);
    }
    break;
  }
  case OBJECT_VARIANT: {
    ObjectVariant *v = (ObjectVariant *)obj;
    gc_mark_object(gc, &v->def->object);
    for (int i = 0; i < v->arity; i++) {
      gc_mark_value(gc, v->payload[i]);
    }
    break;
  }
  case OBJECT_ARRAY: {
    ObjectArray *array = (ObjectArray *)obj;
    gc_mark_values(gc, array->values, array->length);
    break;
  }
  case OBJECT_ARRAY_ITERATOR: {
    ObjectArrayIterator *iter = (ObjectArrayIterator *)obj;
    gc_mark_object(gc, (Object *)iter->array);
    break;
  }
  default:
    assert(false && "unexpected object type");
  }
}

static void gc_trace_references(GarbageCollector *gc) {
  while (array_count(gc->gray_stack) > 0) {
    gc_blacken_object(gc, array_pop(gc->gray_stack));
  }
}

static void gc_sweep(GarbageCollector *gc) {
  Object *prev = NULL;
  Object *obj = gc->vm->objects;
  while (obj != NULL) {
    if (obj->is_marked) {
      obj->is_marked = false;
      prev = obj;
      obj = obj->next;
      continue;
    }

    // obj is unreachable
    Object *unreached = obj;
    obj = obj->next;
    if (prev != NULL) {
      prev->next = obj;
    } else {
      gc->vm->objects = obj;
    }
    obj_free(&unreached, &gc->as_allocator);
  }
}

static void gc_collect(GarbageCollector *gc) {
  if (gc->vm->gc_disabled) {
    return;
  }

#ifdef DEBUG_LOG_GC
  printf("-- gc begin\n");
  size_t before = gc->bytes_allocated;
#endif

  gc_mark_roots(gc);
  gc_trace_references(gc);
  ht_remove_unmarked(&gc->vm->strings);
  gc_sweep(gc);

  gc->next_gc = gc->bytes_allocated * 2;

#ifdef DEBUG_LOG_GC
  printf("-- gc end\n");
  printf("   collected %zu bytes (from %zu to %zu) next at %zu\n",
         before - gc->bytes_allocated, before, gc->bytes_allocated,
         gc->next_gc);
#endif
}

static void *_gc_alloc(void *ctx, size_t size) {
  GarbageCollector *gc = (GarbageCollector *)ctx;
#ifdef DEBUG_STRESS_GC
  gc_collect(gc);
#endif
  gc->bytes_allocated += size;
  if (gc->bytes_allocated >= gc->next_gc) {
    gc_collect(gc);
  }
  return al_alloc(gc->inner, size);
}

static void *_gc_realloc(void *ctx, void *ptr, size_t old_size,
                         size_t new_size) {
  GarbageCollector *gc = (GarbageCollector *)ctx;
#ifdef DEBUG_STRESS_GC
  gc_collect(gc);
#endif
  gc->bytes_allocated += new_size - old_size;
  return al_realloc(gc->inner, ptr, old_size, new_size);
}

static void _gc_free(void *ctx, void *ptr, size_t size) {
  // raise(SIGTRAP);
  GarbageCollector *gc = (GarbageCollector *)ctx;
  gc->bytes_allocated -= size;
  al_free(gc->inner, ptr, size);
}

void gc_init(GarbageCollector *gc, VirtualMachine *vm, Allocator *inner) {
  array_init(gc->gray_stack, Object *, inner);

  gc->bytes_allocated = 0;
  gc->next_gc = GC_INITIAL_THRESHOLD;
  gc->inner = inner;
  gc->vm = vm;
  gc->as_allocator = (Allocator){
      .alloc = _gc_alloc,
      .realloc = _gc_realloc,
      .free = _gc_free,
      .ctx = gc,
  };
}

void gc_destroy(GarbageCollector *gc) { array_free(gc->gray_stack, gc->inner); }

Allocator gc_allocator_create(GarbageCollector *gc) { return gc->as_allocator; }
