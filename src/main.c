#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "bootstrap.h"
#include "garbage_collector.h"
#include "virtual_machine.h"

static void repl(VirtualMachine *vm) {
  char line[1024];

  while (true) {
    printf("> ");

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }

    InterpretResult result = vm_interpret(vm, line);
    if (result == INTERPRET_COMPILE_ERROR) {
      fprintf(stderr, "compile error.\n");
    } else if (result == INTERPRET_RUNTIME_ERROR) {
      fprintf(stderr, "runtime error.\n");
    }
  }
}

static void run_file(VirtualMachine *vm, const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "could not open file '%s'.\n", path);
    exit(74);
  }

  fseek(file, 0L, SEEK_END);
  size_t file_size = ftell(file);
  rewind(file);

  char *buffer = (char *)malloc(file_size + 1);
  if (buffer == NULL) {
    fprintf(stderr, "not enough memory to read '%s'.\n", path);
    exit(74);
  }

  size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
  if (bytes_read < file_size) {
    fprintf(stderr, "could not read file '%s'.\n", path);
    exit(74);
  }

  buffer[bytes_read] = '\0';
  InterpretResult result = vm_interpret(vm, buffer);

  if (result == INTERPRET_COMPILE_ERROR) {
    fprintf(stderr, "compile error.\n");
    exit(65);
  } else if (result == INTERPRET_RUNTIME_ERROR) {
    fprintf(stderr, "runtime error.\n");
    exit(70);
  }

  free(buffer);
}

static VirtualMachine vm;

typedef struct {
  Allocator *inner;
  size_t allocated;
} TrackAllocatorContext;

static void *_track_alloc(void *ctx, size_t size) {
  TrackAllocatorContext *track_ctx = (TrackAllocatorContext *)ctx;

  void *ptr = track_ctx->inner->alloc(track_ctx->inner, size);
  if (ptr) {
    track_ctx->allocated += size;
#ifdef DEBUG_VERBOSE_ALLOC
    printf("alloc: %p %zu bytes (total allocated: %zu)\n", ptr, size,
           track_ctx->allocated);
#endif
  }
  return ptr;
}

static void *_track_realloc(void *ctx, void *ptr, size_t old_size,
                            size_t new_size) {
  TrackAllocatorContext *track_ctx = (TrackAllocatorContext *)ctx;
  void *new_ptr =
      track_ctx->inner->realloc(track_ctx->inner, ptr, old_size, new_size);
  if (new_ptr) {
    track_ctx->allocated += new_size - old_size;
#ifdef DEBUG_VERBOSE_ALLOC
    printf("realloc: %p -> %p (old size: %zu, new size: %zu)\n", ptr, new_ptr,
           old_size, new_size);
#endif
  }
  return new_ptr;
}

static void _track_free(void *ctx, void *ptr, size_t size) {
  TrackAllocatorContext *track_ctx = (TrackAllocatorContext *)ctx;
  track_ctx->allocated -= size;
#ifdef DEBUG_VERBOSE_ALLOC
  printf("free : %p %zu bytes (total allocated: %zu)\n", ptr, size,
         track_ctx->allocated);
#endif
  track_ctx->inner->free(track_ctx->inner, ptr, size);
}

static Allocator track_allocator_create(TrackAllocatorContext *ctx) {
  return (Allocator){
      .alloc = _track_alloc,
      .realloc = _track_realloc,
      .free = _track_free,
      .ctx = ctx,
  };
}

static void ta_init(TrackAllocatorContext *ctx, Allocator *inner) {
  ctx->inner = inner;
  ctx->allocated = 0;
}

static void ta_destory(Allocator *allocator) { (void)allocator; }

int main(int argc, char *argv[]) {
  int ret_code = 0;
  Allocator heap_alloc = heap_allocator_create();

  TrackAllocatorContext track;
  ta_init(&track, &heap_alloc);
  Allocator track_alloc = track_allocator_create(&track);

  GarbageCollector gc;
  gc_init(&gc, &vm, &track_alloc);
  Allocator gc_alloc = gc_allocator_create(&gc);

  vm_init(&vm, gc_alloc);

  if (!bootstrap(&vm)) {
    fprintf(stderr, "bootstrap failed.\n");
    ret_code = 70;
    goto cleanup;
  }

  if (argc == 1) {
    repl(&vm);
  } else if (argc == 2) {
    run_file(&vm, argv[1]);
  } else {
    fprintf(stderr, "usage: interpreter [path]\n");
    ret_code = 64;
    goto cleanup;
  }

cleanup:
  vm_destroy(&vm);

  assert(gc.bytes_allocated == 0 && "memory leak detected: garbage collector "
                                    "has allocated memory that was not freed");
  gc_destroy(&gc);

  assert(track.allocated == 0 && "memory leak detected: track allocator "
                                 "has allocated memory that was not freed");
  ta_destory(&track_alloc);

  return ret_code;
}
