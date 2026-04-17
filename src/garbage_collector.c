#include "garbage_collector.h"

#include <assert.h>

#include "dynamic_array.h"
#include "hash_table.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include "virtual_machine.h"

#ifdef DEBUG_LOG_GC
#include <stdio.h>
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
  obj_print(obj);
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

static void gc_mark_roots(GarbageCollector *gc) {
  for (Value *slot = gc->vm->stack.values; slot < gc->vm->stack.top; slot++) {
    gc_mark_value(gc, *slot);
  }

  const ObjectString *type_names[] = {gc->vm->type_nil, gc->vm->type_bool,
                                      gc->vm->type_number, gc->vm->type_object,
                                      gc->vm->type_empty};
  for (size_t i = 0; i < sizeof(type_names) / sizeof(type_names[0]); i++) {
    gc_mark_object(gc, (Object *)type_names[i]);
  }

  // frame already on stack no need to mark them here
  //   for (int i = 0; i < gc->vm->frame_count; i++) {
  //     CallFrame *frame = &gc->vm->frames[i];
  //     gc_mark_object(gc, (Object *)frame->function);
  //   }

  for (ObjectUpvalue *upvalue = gc->vm->open_upvalues; upvalue != NULL;
       upvalue = upvalue->next) {
    gc_mark_object(gc, (Object *)upvalue);
  }

  gc_mark_hash_table(gc, &gc->vm->globals);
}

static void gc_blacken_object(GarbageCollector *gc, Object *obj) {
#ifdef DEBUG_LOG_GC
  printf("%p blacken ", (void *)obj);
  obj_print(obj);
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
    int method_count = array_count(trait->method_names);
    for (int i = 0; i < method_count; i++) {
      gc_mark_object(gc, (Object *)trait->method_names[i]);
    }
    break;
  }
  case OBJECT_IMPL: {
    ObjectImpl *impl = (ObjectImpl *)obj;
    gc_mark_object(gc, (Object *)impl->trait);
    gc_mark_object(gc, (Object *)impl->struct_def);
    break;
  }
  case OBJECT_TRAIT_OBJECT: {
    ObjectTraitObject *trait_object = (ObjectTraitObject *)obj;
    gc_mark_object(gc, (Object *)trait_object->instance);
    gc_mark_object(gc, (Object *)trait_object->impl);
    break;
  }
  case OBJECT_BOUND_METHOD: {
    ObjectBoundMethod *bound_method = (ObjectBoundMethod *)obj;
    gc_mark_value(gc, bound_method->receiver);
    gc_mark_object(gc, (Object *)bound_method->method);
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
