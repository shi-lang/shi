#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>

#include "../deps/libev/ev.h"
#include "../deps/linenoise.h"
#include "../deps/pcg_basic.h"
#include "../deps/utf8.h"
#include "prelude.inc"

#define OBJ_HM_SIZE 32
static const char *VERSION = "0.1.0";

// {{{ error

void term_disable_raw_mode();

// Globals
#define MAX_ERROR_DEPTH 25
static int error_depth = 0;
static char *error_value;
static jmp_buf error_jmp_env[MAX_ERROR_DEPTH];

static __attribute((noreturn)) void error(char *error_v) {
  error_value = malloc(sizeof(char *) * (strlen(error_v) + 1));
  char *p = stpcpy(error_value, error_v);
  *p = '\0';
  if (error_depth > 0) {
    error_depth--;
    longjmp(error_jmp_env[error_depth], 1);
  }
  term_disable_raw_mode();
  printf("unhandled error: %s\n", error_value);
  exit(1);
}

// }}}

// {{{ type

// value types
enum {
  // Regular values visible from the user
  TINT = 1,
  TSTR,
  TCELL,
  TSYM,
  TOBJ,
  TPRI,
  TFUN,
  TMAC,

  // Intermediary value only present during GC, points to obj in new semispace
  TMOVED,

  // Constants, statically allocated and will never be managed by GC
  TTRUE,
  TNIL,
  TDOT,
  TCPAREN,
  TCCURLY,
};

// primitive fn typedef
struct Val;
typedef struct Val *Primitive(void *root, struct Val **env, struct Val **args);

typedef struct Val {
  // type is used to determine what value is represented in the union
  int type;

  // size is the total allocated size of the object. "type" + "size" +
  // "contents" + extra padding
  int size;

  // value contents
  union {
    // integer
    int intv;
    // string
    char strv[1];
    // list
    struct {
      struct Val *car;
      struct Val *cdr;
    };
    // symbol
    char symv[1];
    // object
    // linked list of association lists containing object properties.
    struct {
      struct Val *proto;
      struct Val *props[];
    };
    // primitive
    Primitive *priv;
    // function or macro
    struct {
      struct Val *params;
      struct Val *body;
      struct Val *env;
    };
    // forwarding pointer (only exists during GC runs)
    void *moved;
  };
} Val;

// Constants
static Val *True = &(Val){TTRUE, 0, {0}};
static Val *Nil = &(Val){TNIL, 0, {0}};
static Val *Dot = &(Val){TDOT, 0, {0}};
static Val *Cparen = &(Val){TCPAREN, 0, {0}};
static Val *Ccurly = &(Val){TCCURLY, 0, {0}};

// The list containing all symbols. Such data structure is traditionally called
// the "obarray", but I avoid using it as a variable name as this is not an
// array but a list.
static Val *symbols;

// }}}

// {{{ types: ev

typedef struct WatcherState {
  int id;
  int type;
  Val *env;
  Val *callback;
} WatcherState;

static ev_watcher_list *ev_watchers = NULL;

static int ev_next_id() {
  static int watcher_id = 0;
  watcher_id++;
  return watcher_id;
}

// }}}

// {{{ memory

// The size of the heap in byte
static const unsigned int MEMORY_SIZE = 67108864; // 64mb

// The pointer pointing to the beginning of the current heap
static void *memory;

// The pointer pointing to the beginning of the old heap
static void *from_space;

// The number of bytes allocated from the heap
static size_t mem_nused = 0;

// Flags to debug GC
static bool gc_running = false;
static bool debug_gc = false;
static bool always_gc = false;

static void gc(void *root);

// Currently we are using Cheney's copying GC algorithm, with which the
// available memory is split into two halves and all objects are moved from one
// half to another every time GC is invoked. That means the address of the
// object keeps changing. If you take the address of an object and keep it in a
// C variable, dereferencing it could cause SEGV because the address becomes
// invalid after GC runs.
//
// In order to deal with that, all access from C to Lisp objects will go through
// two levels of pointer dereferences. The C local variable is pointing to a
// pointer on the C stack, and the pointer is pointing to the Lisp object. GC
// is aware of the pointers in the stack and updates their contents with the
// objects' new addresses when GC happens.
//
// The following is a macro to reserve the area in the C stack for the pointers.
// The contents of this area are considered to be GC root.
//
// Be careful not to bypass the two levels of pointer indirections. If you
// create a direct pointer to an object, it'll cause a subtle bug. Such code
// would work in most cases but fails with SEGV if GC happens during the
// execution of the code. Any code that allocates memory may invoke GC.

#define ROOT_END ((void *)-1)

#define ADD_ROOT(root, size)                                                   \
  void *root_ADD_ROOT_[size + 2];                                              \
  root_ADD_ROOT_[0] = root;                                                    \
  for (int i = 1; i <= size; i++)                                              \
    root_ADD_ROOT_[i] = NULL;                                                  \
  root_ADD_ROOT_[size + 1] = ROOT_END;                                         \
  root = root_ADD_ROOT_

#define DEFINE1(root, var1)                                                    \
  ADD_ROOT(root, 1);                                                           \
  Val **var1 = (Val **)(root_ADD_ROOT_ + 1)

#define DEFINE2(root, var1, var2)                                              \
  ADD_ROOT(root, 2);                                                           \
  Val **var1 = (Val **)(root_ADD_ROOT_ + 1);                                   \
  Val **var2 = (Val **)(root_ADD_ROOT_ + 2)

#define DEFINE3(root, var1, var2, var3)                                        \
  ADD_ROOT(root, 3);                                                           \
  Val **var1 = (Val **)(root_ADD_ROOT_ + 1);                                   \
  Val **var2 = (Val **)(root_ADD_ROOT_ + 2);                                   \
  Val **var3 = (Val **)(root_ADD_ROOT_ + 3)

#define DEFINE4(root, var1, var2, var3, var4)                                  \
  ADD_ROOT(root, 4);                                                           \
  Val **var1 = (Val **)(root_ADD_ROOT_ + 1);                                   \
  Val **var2 = (Val **)(root_ADD_ROOT_ + 2);                                   \
  Val **var3 = (Val **)(root_ADD_ROOT_ + 3);                                   \
  Val **var4 = (Val **)(root_ADD_ROOT_ + 4)

#define DEFINE5(root, var1, var2, var3, var4, var5)                            \
  ADD_ROOT(root, 5);                                                           \
  Val **var1 = (Val **)(root_ADD_ROOT_ + 1);                                   \
  Val **var2 = (Val **)(root_ADD_ROOT_ + 2);                                   \
  Val **var3 = (Val **)(root_ADD_ROOT_ + 3);                                   \
  Val **var4 = (Val **)(root_ADD_ROOT_ + 4);                                   \
  Val **var5 = (Val **)(root_ADD_ROOT_ + 5)

#define DEFINE6(root, var1, var2, var3, var4, var5, var6)                      \
  ADD_ROOT(root, 6);                                                           \
  Val **var1 = (Val **)(root_ADD_ROOT_ + 1);                                   \
  Val **var2 = (Val **)(root_ADD_ROOT_ + 2);                                   \
  Val **var3 = (Val **)(root_ADD_ROOT_ + 3);                                   \
  Val **var4 = (Val **)(root_ADD_ROOT_ + 4);                                   \
  Val **var5 = (Val **)(root_ADD_ROOT_ + 5);                                   \
  Val **var6 = (Val **)(root_ADD_ROOT_ + 6)

// Round up the given value to a multiple of size. Size must be a power of 2. It
// adds size - 1
// first, then zero-ing the least significant bits to make the result a multiple
// of size. I know
// these bit operations may look a little bit tricky, but it's efficient and
// thus frequently used.
static inline size_t roundup(size_t var, size_t size) {
  return (var + size - 1) & ~(size - 1);
}

// Allocates memory block. This may start GC if we don't have enough memory.
static Val *alloc(void *root, int type, size_t size) {
  // The object must be large enough to contain a pointer for the forwarding
  // pointer. Make it larger if it's smaller than that.
  size = roundup(size, sizeof(void *));

  // Add the size of the type tag and size fields.
  size += offsetof(Val, intv);

  // Round up the object size to the nearest alignment boundary, so that the
  // next object will be allocated at the proper alignment boundary. Currently
  // we align the object at the same boundary as the pointer.
  size = roundup(size, sizeof(void *));

  // If the debug flag is on, allocate a new memory space to force all the
  // existing objects to move to new addresses, to invalidate the old addresses.
  // By doing this the GC behavior becomes more predictable and repeatable. If
  // there's a memory bug that the C variable has a direct reference to a Lisp
  // object, the pointer will become invalid by this GC call. Dereferencing that
  // will immediately cause SEGV.
  if (always_gc && !gc_running)
    gc(root);

  // Otherwise, run GC only when the available memory is not large enough.
  if (!always_gc && MEMORY_SIZE < mem_nused + size)
    gc(root);

  // Terminate the program if we couldn't satisfy the memory request. This can
  // happen if the requested size was too large or the from-space was filled
  // with too many live objects.
  if (MEMORY_SIZE < mem_nused + size)
    error("Memory exhausted");

  // Allocate the object.
  Val *obj = memory + mem_nused;
  obj->type = type;
  obj->size = size;
  mem_nused += size;
  return obj;
}

// }}}

// {{{ gc

// Cheney's algorithm uses two pointers to keep track of GC status. At first
// both pointers point to the beginning of the to-space. As GC progresses, they
// are moved towards the end of the to-space. The objects before "scan1" are
// the objects that are fully copied.  The objects between "scan1" and "scan2"
// have already been copied, but may contain pointers to the from-space. "scan2"
// points to the beginning of the free space.
static Val *scan1;
static Val *scan2;

// Moves one object from the from-space to the to-space. Returns the object's
// new address. If the object has already been moved, does nothing but just
// returns
// the new address.
static inline Val *forward(Val *obj) {
  // If the object's address is not in the from-space, the object is not managed
  // by GC nor it has already been moved to the to-space.
  ptrdiff_t offset = (uint8_t *)obj - (uint8_t *)from_space;
  if (offset < 0 || MEMORY_SIZE <= offset)
    return obj;

  // The pointer is pointing to the from-space, but the object there was a
  // tombstone. Follow the forwarding pointer to find the new location of
  // the object.
  if (obj->type == TMOVED)
    return obj->moved;

  // Otherwise, the object has not been moved yet. Move it.
  Val *newloc = scan2;
  memcpy(newloc, obj, obj->size);
  scan2 = (Val *)((uint8_t *)scan2 + obj->size);

  // Put a tombstone at the location where the object used to occupy, so that
  // the following call of forward() can find the object's new location.
  obj->type = TMOVED;
  obj->moved = newloc;
  return newloc;
}

static void *alloc_semispace() {
  // return malloc(MEMORY_SIZE);
  return mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
              -1, 0);
}

static char *pr_str(void *root, Val *);

// Copies the root objects.
static void forward_root_objects(void *root) {
  symbols = forward(symbols);

  // In `root` [0] it a pointer to the previous root, [n] is an object on the
  // stack and [n+1] is the ROOT_END delimiter
  for (void **frame = root; frame; frame = *(void ***)frame) {
    for (int i = 1; frame[i] != ROOT_END; i++) {
      if (frame[i]) {
        frame[i] = forward(frame[i]);
      }
    }
  }

  // Persist/Forward watchers root objects
  for (ev_watcher_list *w = ev_watchers; w != NULL; w = w->next) {
    WatcherState *wdata = w->data;
    wdata->env = forward(wdata->env);
    wdata->callback = forward(wdata->callback);
  }
}

// Implements Cheney's copying garbage collection algorithm.
// http://en.wikipedia.org/wiki/Cheney%27s_algorithm
static void gc(void *root) {
  assert(!gc_running);
  gc_running = true;

  // Allocate a new semi-space.
  from_space = memory;
  memory = alloc_semispace();

  // Initialize the two pointers for GC. Initially they point to the beginning
  // of the to-space.
  scan1 = scan2 = memory;

  // Copy the GC root objects first. This moves the pointer scan2.
  forward_root_objects(root);

  // Copy the objects referenced by the GC root objects located between scan1
  // and scan2. Once it's
  // finished, all live objects (i.e. objects reachable from the root) will have
  // been copied to
  // the to-space.
  while (scan1 < scan2) {
    switch (scan1->type) {
    case TINT:
    case TSTR:
    case TSYM:
    case TPRI:
      // Any of the above types does not contain a pointer to a GC-managed
      // object.
      break;
    case TOBJ:
      scan1->proto = forward(scan1->proto);
      for (size_t i = 0; i < OBJ_HM_SIZE; i++) {
        scan1->props[i] = forward(scan1->props[i]);
      }
      break;
    case TCELL:
      scan1->car = forward(scan1->car);
      scan1->cdr = forward(scan1->cdr);
      break;
    case TFUN:
    case TMAC:
      scan1->params = forward(scan1->params);
      scan1->body = forward(scan1->body);
      scan1->env = forward(scan1->env);
      break;
    default:
      // TODO append scan1->type
      error("bug: copy: unknown type");
    }
    scan1 = (Val *)((uint8_t *)scan1 + scan1->size);
  }

  // Finish up GC.
  // free(from_space);
  munmap(from_space, MEMORY_SIZE);
  size_t old_nused = mem_nused;
  mem_nused = (size_t)((uint8_t *)scan1 - (uint8_t *)memory);
  if (debug_gc)
    fprintf(stderr, "GC: %zu bytes out of %zu bytes copied.\n", mem_nused,
            old_nused);
  gc_running = false;
}

// }}}

// {{{ constructors

static Val *make_int(void *root, int value) {
  Val *r = alloc(root, TINT, sizeof(int));
  r->intv = value;
  return r;
}

static Val *make_str(void *root, char *value) {
  Val *str = alloc(root, TSTR, strlen(value) + 1);
  strcpy(str->strv, value);
  return str;
}

static Val *make_symbol(void *root, char *name) {
  Val *sym = alloc(root, TSYM, strlen(name) + 1);
  strcpy(sym->symv, name);
  return sym;
}

// {{{ constructors: list

// cell/list constructor
static Val *cons(void *root, Val **car, Val **cdr) {
  Val *cell = alloc(root, TCELL, sizeof(Val *) * 2);
  cell->car = *car;
  cell->cdr = *cdr;
  return cell;
}

// Returns the length of the given list. -1 if it's not a proper list.
static int length(Val *list) {
  int len = 0;
  for (; list->type == TCELL; list = list->cdr)
    len++;
  return list == Nil ? len : -1;
}

// Destructively reverses the given list.
static Val *reverse(Val *p) {
  Val *ret = Nil;
  while (p != Nil) {
    Val *head = p;
    p = p->cdr;
    head->cdr = ret;
    ret = head;
  }
  return ret;
}

// Returns ((x . y) . a)
static Val *acons(void *root, Val **x, Val **y, Val **a) {
  DEFINE1(root, cell);
  *cell = cons(root, x, y);
  return cons(root, cell, a);
}

// }}}

// {{{ constructors: obj

static void obj_set(void *, Val **, Val **, Val **);

static Val *make_obj(void *root, Val **proto) {
  Val *r = alloc(root, TOBJ, sizeof(Val *) * (OBJ_HM_SIZE + 1));
  r->proto = *proto;
  for (size_t i = 0; i < OBJ_HM_SIZE; i++) {
    r->props[i] = Nil;
  }
  return r;
}

static Val *make_obj_alist(void *root, Val **proto, Val **props) {
  DEFINE3(root, obj, key, val);
  *obj = make_obj(root, proto);
  for (Val *pair = *props; pair != Nil; pair = pair->cdr) {
    *key = pair->car->car;
    *val = pair->car->cdr;
    obj_set(root, obj, key, val);
  }
  return *obj;
}

static size_t obj_hash(Val *key) {
  u_int64_t hash;
  size_t i = 0;

  char *keyval;
  if (key->type == TSTR) {
    keyval = key->strv;
  } else if (key->type == TSYM) {
    keyval = key->symv;
  } else if (key->type == TINT) {
    char buf[11];
    memset(buf, 0, 11);
    sprintf(buf, "%d", key->intv);
    keyval = buf;
  } else {
    error("obj_hash: key given is not sym, str, or int");
  }

  size_t keylen = strlen(keyval);

  // http://en.wikipedia.org/wiki/Jenkins_hash_function
  for (hash = i = 0; i < keylen; ++i) {
    hash += keyval[i];
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);

  return hash % OBJ_HM_SIZE;
}

static bool obj_valid_key(Val *key) {
  size_t t = key->type;
  return t == TSYM || t == TSTR || t == TINT;
}

static bool obj_key_eq(Val *a, Val *b) {
  if (a->type == TSYM && b->type == TSYM) {
    return a == b;
  } else if (a->type == TINT && b->type == TINT) {
    return a->intv == b->intv;
  } else if (a->type == TSTR && b->type == TSTR) {
    return strcmp(a->strv, b->strv) == 0;
  } else {
    return false;
  }
}

// Gets the alist cell for k in obj (or NULL)
// Take an already computed hash (used in obj_set)
static Val *_obj_get(Val *obj, size_t h, Val *k) {
  Val *list = obj->props[h];
  for (Val *pair = list; pair != Nil; pair = pair->cdr) {
    if (obj_key_eq(k, pair->car->car)) {
      return pair->car;
    }
  }
  return NULL;
}

// Gets the alist cell for k in obj (or NULL)
static Val *obj_get(Val *obj, Val *k) {
  return _obj_get(obj, obj_hash(k), k);
}

// Gets the alist cell for k in obj or any of it's prototypes
static Val *obj_find(Val *obj, Val *k) {
  size_t h = obj_hash(k);
  for (Val *o = obj; o != Nil; o = o->proto) {
    Val *v = _obj_get(o, h, k);
    if (v != NULL) {
      return v;
    }
  }
  return NULL;
}

// Set object key to value
static void obj_set(void *root, Val **obj, Val **key, Val **val) {
  DEFINE2(root, list, pair);

  size_t h = obj_hash(*key);
  *pair = _obj_get(*obj, h, *key);
  *list = (*obj)->props[h];

  if (*pair == NULL) {
    // Not found, insert
    *pair = cons(root, key, val);
    *pair = cons(root, pair, list);
    (*obj)->props[h] = *pair;
  } else {
    // Found, set-cdr
    (*pair)->cdr = *val;
  }
}

// Remove a k/v from object
static void obj_del(Val *obj, Val *k) {
  size_t h = obj_hash(k);
  Val **list = &obj->props[h];

  for (Val **pair = list; (*pair) != Nil; *pair = (*pair)->cdr) {
    if (obj_key_eq(k, (*pair)->car->car)) {
      *pair = (*pair)->cdr;
    }
  }
}

// }}}

static Val *make_primitive(void *root, Primitive *fn) {
  Val *r = alloc(root, TPRI, sizeof(Primitive *));
  r->priv = fn;
  return r;
}

static Val *make_function(void *root, Val **env, int type, Val **params,
                          Val **body) {
  assert(type == TFUN || type == TMAC);
  Val *r = alloc(root, type, sizeof(Val *) * 3);
  r->params = *params;
  r->body = *body;
  r->env = *env;
  return r;
}

// }}}

// {{{ util + pretty-print

// May create a new symbol. If there's a symbol with the same name, it will not
// create a new symbol but return the existing one.
static Val *intern(void *root, char *name) {
  for (Val *p = symbols; p != Nil; p = p->cdr)
    if (strcmp(name, p->car->symv) == 0)
      return p->car;
  DEFINE1(root, sym);
  *sym = make_symbol(root, name);
  symbols = cons(root, sym, &symbols);
  return *sym;
}

#define PP_MAX_LEN 16384
static char *pr_str(void *root, Val *obj) {

  char *buf = malloc(sizeof(char) * PP_MAX_LEN);
  bzero(buf, sizeof(char) * PP_MAX_LEN);
  char *s;
  Val *val;
  int len = 0;

  switch (obj->type) {
  case TCELL:
    len += sprintf(&buf[len], "(");
    for (;;) {
      s = pr_str(root, obj->car);
      len += sprintf(&buf[len], "%s", s);
      free(s);
      if (obj->cdr == Nil)
        break;
      if (obj->cdr->type != TCELL) {
        len += sprintf(&buf[len], " . ");
        s = pr_str(root, obj->cdr);
        len += sprintf(&buf[len], "%s", s);
        free(s);
        break;
      }
      len += sprintf(&buf[len], " ");
      obj = obj->cdr;
    }
    len += sprintf(&buf[len], ")");
    return buf;
  case TSTR:
    len += sprintf(&buf[len], "\"");
    len += u8_escape(&buf[len], PP_MAX_LEN - len, obj->strv, '"');
    len += sprintf(&buf[len], "\"");
    return buf;
  case TOBJ:
    val = obj_find(obj, intern(root, "*object-name*"));
    if (val != NULL && val->cdr->type == TSTR) {
      len += sprintf(&buf[len], "<object %s %p>", val->cdr->strv, obj);
    } else {
      len += sprintf(&buf[len], "<object %s %p>", "nil", obj);
    }
    return buf;

#define CASE(type, ...)                                                        \
  case type:                                                                   \
    len += sprintf(&buf[len], __VA_ARGS__);                                    \
    return buf

    CASE(TINT, "%d", obj->intv);
    CASE(TSYM, "%s", obj->symv);
    CASE(TPRI, "<primitive>");
    CASE(TFUN, "<function>");
    CASE(TMAC, "<macro>");
    CASE(TMOVED, "<moved>");
    CASE(TTRUE, "t");
    CASE(TNIL, "()");

#undef CASE

  default:
    // DEBUG
    // len += sprintf(&buf[len], "<tag %d>", obj->type);
    // return buf;
    free(buf);
    // TODO append obj->type
    error("Bug: print: Unknown tag type");
  }
}

int setnonblock(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL);
  flags |= O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}

// }}}

// {{{ reader

#define SYMBOL_MAX_LEN 200
#define STRING_MAX_LEN 1000
const char symbol_chars[] = "~!#$%^&*-_=+:/?<>";

static bool valid_symbol_start_char(char c) {
  return (isalpha(c) || strchr(symbol_chars, c)) && c != '\0';
}

static bool valid_symbol_char(char c) {
  return (isalnum(c) || strchr(symbol_chars, c)) && c != '\0';
}

typedef struct Reader {
  int pos;
  int size;
  char *input;
} Reader;

static Val *reader_expr(Reader *r, void *root);

static Reader *reader_new(char *input) {
  Reader *r = malloc(sizeof(Reader));
  r->pos = -1;
  r->size = strlen(input);
  r->input = malloc(sizeof(char) * (r->size + 1));
  strcpy(r->input, input);
  return r;
}

static void reader_destroy(Reader *r) {
  free(r->input);
  free(r);
}

static int reader_peek(Reader *r) {
  if ((r->pos + 1) == r->size) {
    return EOF;
  }
  return r->input[r->pos + 1];
}

static int reader_next(Reader *r) {
  r->pos++;
  if (r->pos == r->size) {
    return EOF;
  }
  return r->input[r->pos];
}

// Skips the input until newline is found. Newline is one of \r, \r\n or \n.
static void reader_skip_line(Reader *r) {
  for (;;) {
    int c = reader_next(r);
    if (c == EOF || c == '\n') {
      return;
    }
    if (c == '\r') {
      if (reader_peek(r) == '\n') {
        reader_next(r);
      }
      return;
    }
  }
}

// Reads a list. Note that '(' has already been read.
static Val *reader_list(Reader *r, void *root) {
  DEFINE3(root, obj, head, last);
  *head = Nil;
  for (;;) {
    *obj = reader_expr(r, root);
    if (!*obj)
      error("Unclosed parenthesis");
    if (*obj == Cparen)
      return reverse(*head);
    if (*obj == Dot) {
      *last = reader_expr(r, root);
      if (reader_expr(r, root) != Cparen)
        error("Closed parenthesis expected after dot");
      Val *ret = reverse(*head);
      (*head)->cdr = *last;
      return ret;
    }
    *head = cons(root, obj, head);
  }
}

// Reads an alist. Note that '{' has already been read.
static Val *reader_alist(Reader *r, void *root) {
  DEFINE6(root, obj, head, ahead, pair, list_sym, cons_sym);
  *head = Nil;

  for (;;) {
    *obj = reader_expr(r, root);
    if (!*obj)
      error("Unclosed curly brace");
    if (*obj == Dot)
      error("Stray dot in alist");
    if (*obj == Cparen)
      error("Stray closing parent in alist");
    if (*obj == Ccurly) {
      if (length(*head) % 2 != 0) {
        error("Alist contains un-even number of elements");
      }
      if (*head == Nil) {
        return Nil;
      }

      *ahead = Nil;
      *list_sym = intern(root, "list");
      *cons_sym = intern(root, "cons");
      do {
        // Pop the two last items (value first as they are reversed)
        *obj = (*head)->car;
        *pair = cons(root, obj, &Nil);
        *obj = (*head)->cdr->car;
        *pair = cons(root, obj, pair);
        *pair = cons(root, cons_sym, pair);
        *head = (*head)->cdr->cdr;

        *ahead = cons(root, pair, ahead);
      } while (*head != Nil);
      *ahead = reverse(*ahead);
      return cons(root, list_sym, ahead);
    }

    *head = cons(root, obj, head);
  }
}

// 'def -> (quote def)
// `(list a) -> (quasiquote (list a))
// @b -> (unbox b)
static Val *read_special(Reader *r, void *root, char *name) {
  DEFINE2(root, sym, tmp);
  *sym = intern(root, name);
  *tmp = reader_expr(r, root);
  *tmp = cons(root, tmp, &Nil);
  *tmp = cons(root, sym, tmp);
  return *tmp;
}

static Val *read_unquote(Reader *r, void *root) {
  DEFINE2(root, sym, tmp);
  if (reader_peek(r) == '@') {
    reader_next(r);
    *sym = intern(root, "unquote-splicing");
  } else {
    *sym = intern(root, "unquote");
  }
  *tmp = reader_expr(r, root);
  *tmp = cons(root, tmp, &Nil);
  *tmp = cons(root, sym, tmp);
  return *tmp;
}

static int read_number(Reader *r, int val) {
  while (isdigit(reader_peek(r))) {
    val = val * 10 + (reader_next(r) - '0');
  }
  return val;
}

static Val *read_string(Reader *r, void *root) {
  char buf[STRING_MAX_LEN + 1];
  int len = 0;
  while (reader_peek(r) != '"' || buf[len - 1] == '\\') {
    if (STRING_MAX_LEN <= len) {
      error("String too long");
    }
    buf[len++] = reader_next(r);
  }
  buf[len] = '\0';

  int buf_len = strlen(buf) + 1;
  char unescaped_buf[buf_len];
  u8_unescape(unescaped_buf, buf_len, buf);

  // consume closing "
  reader_next(r);

  // create str
  DEFINE1(root, tmp);
  *tmp = make_str(root, unescaped_buf);
  return *tmp;
}

static Val *read_symbol(Reader *r, void *root, char c) {
  char buf1[SYMBOL_MAX_LEN + 1];
  char buf2[SYMBOL_MAX_LEN + 1];
  bool found_colon = false;
  int len = 1;
  buf1[0] = c;

  while (valid_symbol_char(reader_peek(r))) {
    if (SYMBOL_MAX_LEN <= len) {
      error("Symbol name too long");
    }
    if (!found_colon) {
      // Normal case
      buf1[len++] = reader_next(r);

      // Found colon syntax, split up
      if (buf1[len - 1] == ':') {
        buf1[len - 1] = '\0';
        len = 0;
        found_colon = true;
      }
    } else {
      // Building second part of object get syntax
      buf2[len++] = reader_next(r);
    }
  }

  if (found_colon && len > 0) {
    buf2[len] = '\0';
    DEFINE5(root, expr, quote_sym, colon_sym, obj_sym, prop_sym);
    *quote_sym = intern(root, "quote");
    *colon_sym = intern(root, ":");
    *obj_sym = intern(root, buf1);
    *prop_sym = intern(root, buf2);
    *expr = cons(root, prop_sym, &Nil);
    *expr = cons(root, quote_sym, expr);
    *expr = cons(root, expr, &Nil);
    *expr = cons(root, obj_sym, expr);
    *expr = cons(root, colon_sym, expr);
    return *expr;
  }

  buf1[len] = '\0';
  return intern(root, buf1);
}

static Val *reader_expr(Reader *r, void *root) {
  for (;;) {
    int c = reader_next(r);
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
      continue;
    if (c == EOF)
      return NULL;
    if (c == ';' || (r->pos == 0 && c == '#')) {
      reader_skip_line(r);
      continue;
    }
    if (c == '(')
      return reader_list(r, root);
    if (c == ')')
      return Cparen;
    if (c == '{')
      return reader_alist(r, root);
    if (c == '}')
      return Ccurly;
    if (c == '.')
      return Dot;
    if (c == '@')
      return read_special(r, root, "unbox");
    if (c == '\'')
      return read_special(r, root, "quote");
    if (c == '`')
      return read_special(r, root, "quasiquote");
    if (c == ',') // handle ,@ too
      return read_unquote(r, root);
    if (c == '"')
      return read_string(r, root);
    if (isdigit(c))
      return make_int(root, read_number(r, c - '0'));
    if (c == '-' && isdigit(reader_peek(r)))
      return make_int(root, -read_number(r, 0));
    if (valid_symbol_start_char(c))
      return read_symbol(r, root, c);

    // TODO cleanup
    char *err_text = "Don't know how to handle ";
    int err_text_len = strlen(err_text);
    char err_buf[err_text_len + 2];
    strncpy(err_buf, err_text, err_text_len);
    err_buf[err_text_len] = c;
    err_buf[err_text_len + 1] = '\0';
    error(err_buf);
  }
}

// }}}

// {{{ eval

static Val *eval(void *root, Val **env, Val **obj);

// Adds a variable to the current env level
static void env_set(void *root, Val **env, Val **sym, Val **val) {
  obj_set(root, env, sym, val);
}

// Searches for a variable by symbol. Returns null if not found.
static Val *env_get(Val **env, Val *sym) {
  return obj_find(*env, sym);
}

// Returns a newly created environment frame.
static Val *push_env(void *root, Val **env, Val **vars, Val **vals) {
  DEFINE3(root, map, sym, val);
  *map = Nil;
  if ((*vars)->type == TSYM) {
    // (fn xs body ...)
    *map = acons(root, vars, vals, map);
  } else {
    // (fn (x y) body ...)
    for (; (*vars)->type == TCELL; *vars = (*vars)->cdr, *vals = (*vals)->cdr) {
      if ((*vals)->type != TCELL)
        error("Cannot apply function: number of argument does not match");
      *sym = (*vars)->car;
      *val = (*vals)->car;
      *map = acons(root, sym, val, map);
    }
    if (*vars != Nil)
      *map = acons(root, vars, vals, map);
  }
  return make_obj_alist(root, env, map);
}

// Evaluates the list elements from head and returns the last return value.
static Val *progn(void *root, Val **env, Val **list) {
  DEFINE2(root, lp, r);
  *r = Nil;
  for (*lp = *list; *lp != Nil; *lp = (*lp)->cdr) {
    *r = (*lp)->car;
    *r = eval(root, env, r);
  }
  return *r;
}

// Evaluates all the list elements and returns their return values as a new
// list.
static Val *eval_list(void *root, Val **env, Val **list) {
  DEFINE4(root, head, lp, expr, result);
  *head = Nil;
  for (lp = list; *lp != Nil; *lp = (*lp)->cdr) {
    *expr = (*lp)->car;
    *result = eval(root, env, expr);
    *head = cons(root, result, head);
  }
  return reverse(*head);
}

static bool is_list(Val *obj) { return obj == Nil || obj->type == TCELL; }

static Val *apply_func(void *root, Val **env, Val **fn, Val **args) {
  (void)env;
  DEFINE3(root, params, newenv, body);
  *params = (*fn)->params;
  *newenv = (*fn)->env;
  *newenv = push_env(root, newenv, params, args);
  *body = (*fn)->body;
  return progn(root, newenv, body);
}

// Apply fn with args.
static Val *apply(void *root, Val **env, Val **fn, Val **args, bool do_eval) {
  if (!is_list(*args)) {
    error("apply: argument must be a list");
  }
  if ((*fn)->type == TPRI)
    return (*fn)->priv(root, env, args);
  if ((*fn)->type == TFUN) {
    DEFINE1(root, eargs);
    if (do_eval) {
      *eargs = eval_list(root, env, args);
    } else {
      *eargs = *args;
    }
    return apply_func(root, env, fn, eargs);
  }
  error("apply: not supported");
}

// Expands the given macro application form.
static Val *macroexpand(void *root, Val **env, Val **val) {
  if ((*val)->type != TCELL ||
      ((*val)->car->type != TSYM && (*val)->car->type != TMAC)) {
    return *val;
  }
  DEFINE3(root, bind, macro, args);
  if ((*val)->car->type == TMAC) {
    *macro = (*val)->car;
  } else {
    *bind = env_get(env, (*val)->car);
    if (!*bind || (*bind)->cdr->type != TMAC)
      return *val;
    *macro = (*bind)->cdr;
  }
  *args = (*val)->cdr;
  return apply_func(root, env, macro, args);
}

// Evaluates the S expression.
static Val *eval(void *root, Val **env, Val **obj) {
  switch ((*obj)->type) {
  case TINT:
  case TSTR:
  case TOBJ:
  case TPRI:
  case TFUN:
  case TMAC:
  case TTRUE:
  case TNIL:
    // Self-evaluating objects
    return *obj;
  case TSYM: {
    // Variable
    if (*obj == intern(root, "*env*")) {
      return *env;
    }
    Val *bind = env_get(env, *obj);
    if (bind == NULL) {
      // TODO clean up string messing
      char *err_text = "eval: undefined symbol: ";
      char *err_val = (*obj)->symv;
      int err_text_len = strlen(err_text);
      int err_val_len = strlen(err_val);
      char err_buf[err_text_len + err_val_len + 1];
      strncpy(err_buf, err_text, err_text_len);
      strncpy(&err_buf[err_text_len], err_val, err_val_len);
      err_buf[err_text_len + err_val_len] = '\0';
      error(err_buf);
    }
    return bind->cdr;
  }
  case TCELL: {
    // Function application form
    DEFINE3(root, fn, expanded, args);
    *expanded = macroexpand(root, env, obj);
    if (*expanded != *obj)
      return eval(root, env, expanded);
    *fn = (*obj)->car;
    *fn = eval(root, env, fn);
    *args = (*obj)->cdr;
    if ((*fn)->type != TPRI && (*fn)->type != TFUN) {
      error("The head of a list must be a function");
    }
    return apply(root, env, fn, args, true);
  }
  default:
    // TODO append (*obj)->type
    error("Bug: eval: Unknown tag type");
  }
}

// }}}

// {{{ primitives

// {{{ primitives: language

// (do body ...)
static Val *prim_do(void *root, Val **env, Val **list) {
  return progn(root, env, list);
}

// (while cond expr ...)
static Val *prim_while(void *root, Val **env, Val **list) {
  if (length(*list) < 2)
    error("Malformed while");
  DEFINE2(root, cond, exprs);
  *cond = (*list)->car;
  while (eval(root, env, cond) != Nil) {
    *exprs = (*list)->cdr;
    eval_list(root, env, exprs);
  }
  return Nil;
}

static Val *handle_function(void *root, Val **env, Val **list, int type) {
  if ((*list)->type != TCELL ||
      !(is_list((*list)->car) || (*list)->car->type == TSYM) ||
      (*list)->cdr->type != TCELL) {
    // TODO append pr_str(root, *list)
    error("Malformed fn or macro");
  }

  DEFINE2(root, params, body);
  *params = (*list)->car;
  *body = (*list)->cdr;
  Val *p = *params;

  // validate (arg0 arg1) or (arg0 . argN) forms
  if (p->type != TSYM) { // but allow a single symbol to be params
    for (; p->type == TCELL; p = p->cdr)
      if (p->car->type != TSYM)
        error("fn|macro: arg list must contain only symbols");
    if (p != Nil && p->type != TSYM)
      error("fn|macro: arg list must contain only symbols");
  }

  return make_function(root, env, type, params, body);
}

// (fn (<symbol> ...) expr ...)
static Val *prim_fn(void *root, Val **env, Val **list) {
  return handle_function(root, env, list, TFUN);
}

// (macro (<symbol> ...) expr ...)
static Val *prim_macro(void *root, Val **env, Val **list) {
  return handle_function(root, env, list, TMAC);
}

// (def <symbol> expr)
static Val *prim_def(void *root, Val **env, Val **list) {
  if (length(*list) != 2 || (*list)->car->type != TSYM)
    error("Malformed def");
  DEFINE2(root, sym, value);
  *sym = (*list)->car;
  *value = (*list)->cdr->car;
  *value = eval(root, env, value);
  env_set(root, env, sym, value);
  return *value;
}

static Val *prim_def_global(void *root, Val **env, Val **list) {
  if (length(*list) != 2 || (*list)->car->type != TSYM)
    error("Malformed def-global");
  DEFINE2(root, sym, value);
  *sym = (*list)->car;
  *value = (*list)->cdr->car;
  *value = eval(root, env, value);
  while ((*env)->proto != Nil) {
    *env = (*env)->proto;
  }
  env_set(root, env, sym, value);
  return *value;
}

// (set <symbol> expr) or (set (: obj key) val)
static Val *prim_set(void *root, Val **env, Val **list) {
  DEFINE3(root, key, val, obj);
  if (length(*list) != 2)
    error("Malformed set");

  // Check for obj-set syntax (set (: obj key) val)
  if ((*list)->car->type == TCELL && length((*list)->car) == 3 &&
      (*list)->car->car->type == TSYM && (*list)->car->car->symv[0] == ':') {
    *obj = (*list)->car->cdr->car;
    *obj = eval(root, env, obj);
    *key = (*list)->car->cdr->cdr->car;
    *key = eval(root, env, key);
    *val = (*list)->cdr->car;
    *val = eval(root, env, val);

    if ((*obj)->type != TOBJ)
      error("set: (:) 1st arg is not an object");
    if ((*key)->type != TSYM)
      error("set: (:) 2nd arg is not a symbol");

    obj_set(root, obj, key, val);
    return *obj;
  }

  if ((*list)->car->type != TSYM)
    error("Malformed set");
  *key = env_get(env, (*list)->car);
  if (!*key) {
    // TODO append (*list)->car->symv
    error("Unbound variable");
  }
  *val = (*list)->cdr->car;
  *val = eval(root, env, val);
  (*key)->cdr = *val;
  return *val;
}

// (pr-str expr)
static Val *prim_pr_str(void *root, Val **env, Val **list) {
  DEFINE2(root, tmp, s);
  *tmp = (*list)->car;
  char *str = pr_str(root, eval(root, env, tmp));
  *s = make_str(root, str);
  return *s;
}

// (if expr expr expr ...)
static Val *prim_if(void *root, Val **env, Val **list) {
  if (length(*list) < 2)
    error("Malformed if");
  DEFINE3(root, cond, then, els);
  *cond = (*list)->car;
  *cond = eval(root, env, cond);
  if (*cond != Nil) {
    // Test succeded, return then branch and skip evaluatin else
    *then = (*list)->cdr->car;
    return eval(root, env, then);
  }
  *els = (*list)->cdr->cdr;
  if (*els == Nil) {
    // Return nil when else is missing
    return Nil;
  }
  if ((*els)->cdr == Nil) {
    // Return else value if it's last in args (if test then else)
    *then = (*els)->car;
    return eval(root, env, then);
  }
  // Re-enter if with else branch as start (if a ar b br ...)
  return prim_if(root, env, els);
}

// (eq? expr expr)
static Val *prim_eq(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("eq?: needs exactly 2 arguments");
  Val *values = eval_list(root, env, list);
  if (obj_key_eq(values->car, values->cdr->car)) {
    return True;
  }
  return values->car == values->cdr->car ? True : Nil;
}

// (type expr)
static Val *prim_type(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("type: not given exactly 1 argument");
  Val *values = eval_list(root, env, list);

  char *name;

  switch (values->car->type) {
  case TTRUE:
    name = "true";
    break;
  case TNIL:
    name = "nil";
    break;
  case TINT:
    name = "int";
    break;
  case TSTR:
    name = "str";
    break;
  case TSYM:
    name = "sym";
    break;
  case TOBJ:
    name = "obj";
    break;
  case TPRI:
    name = "prim";
    break;
  case TFUN:
    name = "fn";
    break;
  case TMAC:
    name = "macro";
    break;
  case TCELL:
    if (values->car->cdr != Nil && values->car->cdr->type != TCELL) {
      name = "cons";
    } else {
      name = "list";
    }
    break;
  default:
    // TODO append values->car->type
    error("type: unknown object type");
  }

  DEFINE1(root, k);
  *k = intern(root, name);
  return *k;
}

// (apply fn args)
static Val *prim_apply(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("apply: not given exactly 2 args");
  DEFINE2(root, fn, args);
  *fn = (*list)->car;
  *fn = eval(root, env, fn);

  *args = (*list)->cdr->car;
  *args = eval(root, env, args);
  if ((*args)->type != TCELL && *args != Nil)
    error("apply: 2nd argument is not a list");

  return apply(root, env, fn, args, false);
}

// (eval expr)
static Val *prim_eval(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("Malformed eval");
  DEFINE1(root, arg);
  *arg = (*list)->car;
  *arg = eval(root, env, arg); // arg to ast
  return eval(root, env, arg); // ast to value
}

// (read-sexp str)
static Val *prim_read_sexp(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("read-sexp: exactly 1 param required");
  DEFINE4(root, str, expr, exprs, do_sym);
  *str = (*list)->car;
  *str = eval(root, env, str);
  if ((*str)->type != TSTR)
    error("read-sexp: 1st arg is not a string");

  Reader *r = reader_new((*str)->strv);
  *exprs = Nil;

  for (;;) {
    *expr = reader_expr(r, root);
    if (!*expr) {
      reader_destroy(r);

      // Finalize.
      // For single expressions simply return it
      // Wrap multiple expressions in a (do ...)
      if (length(*exprs) == 1) {
        return (*exprs)->car;
      } else {
        *do_sym = intern(root, "do");
        *exprs = reverse(*exprs);
        return cons(root, do_sym, exprs);
      }
    } else if (*expr == Cparen) {
      reader_destroy(r);
      error("Stray close parenthesis");
    } else if (*expr == Ccurly) {
      reader_destroy(r);
      error("Stray close curly bracket");
    } else if (*expr == Dot) {
      reader_destroy(r);
      error("Stray dot");
    }
    *exprs = cons(root, expr, exprs);
  }
}

// (sym str)
static Val *prim_sym(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("sym: exactly 1 param required");
  DEFINE1(root, str);
  *str = (*list)->car;
  *str = eval(root, env, str);
  if ((*str)->type != TSTR)
    error("sym: 1st arg is not a string");

  return intern(root, (*str)->strv);
}

// }}}

// {{{ primitives: marco

// (quote expr)
static Val *prim_quote(void *root, Val **env, Val **list) {
  (void)root;
  (void)env;
  if (length(*list) != 1)
    error("Malformed quote");
  return (*list)->car;
}

// (macro-expand expr)
static Val *prim_macro_expand(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("Malformed macro-expand");
  DEFINE1(root, body);
  *body = (*list)->car;
  *body = eval(root, env, body);
  return macroexpand(root, env, body);
}

// (gensym)
static Val *prim_gensym(void *root, Val **env, Val **list) {
  (void)env;
  (void)list;
  static int count = 0;
  char buf[16];
  snprintf(buf, sizeof(buf), "G__%d", count++);
  return make_symbol(root, buf);
}

// }}}

// {{{ primitives: object

// (obj proto props) ; nil|obj -> alist -> obj
static Val *prim_obj(void *root, Val **env, Val **list) {
  // We have 2 args?
  if (length(*list) != 2) {
    error("obj: expected exactly 2 args");
  }

  // 1st arg is nil or an object?
  Val *args = eval_list(root, env, list);
  if (args->car->type != TOBJ && args->car != Nil) {
    error("obj: given non object or nil as prototype");
  }

  // 2nd arg is a list?
  if (args->cdr->car->type != TCELL && args->cdr->car != Nil) {
    error("obj: given non alist as properties");
  }

  // 2nd arg is an association list
  for (Val *i = args->cdr->car; i != Nil; i = i->cdr) {
    if (i->type != TCELL || i->car->cdr == Nil) {
      error("obj: given non alist as properties");
    } else if (i->car->car->type != TSYM) {
      error("obj: given non symbol as property key");
    }
  }

  DEFINE3(root, obj, proto, props);
  *proto = args->car;
  *props = args->cdr->car;

  *obj = make_obj_alist(root, proto, props);
  return *obj;
}

static Val *prim_obj_get(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("obj-get: expected exactly 2 args");
  Val *args = eval_list(root, env, list);
  if (args->car->type != TOBJ)
    error("obj-get: expected 1st argument to be object");
  if (args->cdr->car->type != TSYM)
    error("obj-get: expected 2nd argument to be symbol");

  DEFINE3(root, o, k, value);
  *o = args->car;
  *k = args->cdr->car;
  *value = obj_find(*o, *k);
  if (*value == NULL) {
    // TODO append args->cdr->car->symv
    error("obj-get: unbound symbol");
  }

  return (*value)->cdr;
}

static Val *prim_obj_set(void *root, Val **env, Val **list) {
  if (length(*list) != 3)
    error("obj-set: expected exactly 3 args");
  Val *args = eval_list(root, env, list);
  if (args->car->type != TOBJ)
    error("obj-set: expected 1st argument to be object");
  if (!obj_valid_key(args->cdr->car))
    error("obj-set: expected 2nd argument to be valid object key");

  DEFINE3(root, obj, key, val);
  *obj = args->car;
  *key = args->cdr->car;
  *val = args->cdr->cdr->car;
  obj_set(root, obj, key, val);

  return *obj;
}

static Val *prim_obj_del(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("obj-del: expected exactly 2 args");
  Val *args = eval_list(root, env, list);
  if (args->car->type != TOBJ)
    error("obj-del: expected 1st argument to be object");
  if (obj_valid_key(args->cdr->car))
    error("obj-del: expected 2nd argument to be valid object key");

  Val *obj = args->car;
  Val *key = args->cdr->car;
  obj_del(obj, key);

  return obj;
}

static Val *prim_obj_proto(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("obj-proto: expected exactly 1 args");
  Val *args = eval_list(root, env, list);
  if (args->car->type != TOBJ)
    error("obj-proto: expected 1st argument to be object");

  return args->car->proto;
}

static Val *prim_obj_proto_set(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("obj-proto-set!: expected exactly 2 args");
  Val *args = eval_list(root, env, list);
  if (args->car->type != TOBJ)
    error("obj-proto-set!: expected 1st argument to be object");

  args->car->proto = args->cdr->car;
  return args->car;
}

static Val *prim_obj_to_alist(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("obj->alist: expected exactly 1 arg");
  Val *args = eval_list(root, env, list);
  if (args->car->type != TOBJ)
    error("obj->alist: expected 1st argument to be object");

  DEFINE2(root, alist, pair);
  *alist = Nil;

  for (size_t i = 0; i < OBJ_HM_SIZE; i++) {
    for (Val *l = args->car->props[i]; l != Nil; l = l->cdr) {
      *pair = l->car;
      *alist = cons(root, pair, alist);
    }
  }

  return *alist;
}

// }}}

// {{{ primitives: list

// (cons expr expr)
static Val *prim_cons(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("Malformed cons");
  Val *cell = eval_list(root, env, list);
  cell->cdr = cell->cdr->car;
  return cell;
}

// (car <cell>)
static Val *prim_car(void *root, Val **env, Val **list) {
  Val *args = eval_list(root, env, list);
  if (args->car->type != TCELL || args->cdr != Nil)
    error("Malformed car");
  return args->car->car;
}

// (cdr <cell>)
static Val *prim_cdr(void *root, Val **env, Val **list) {
  Val *args = eval_list(root, env, list);
  if (args->car->type != TCELL || args->cdr != Nil)
    error("Malformed cdr");
  return args->car->cdr;
}

// (set-car! <cell> expr)
static Val *prim_set_car(void *root, Val **env, Val **list) {
  DEFINE1(root, args);
  *args = eval_list(root, env, list);
  if (length(*args) != 2 || (*args)->car->type != TCELL)
    error("set_car!: invalid arguments");
  (*args)->car->car = (*args)->cdr->car;
  return (*args)->car;
}

// }}}

// {{{ primitives: string

// (str str0 str1 str3)
static Val *prim_str(void *root, Val **env, Val **list) {
  // Ensure we are only dealing with strings and compute final length
  int len = 0;
  Val *args = eval_list(root, env, list);
  for (Val *a = args; a != Nil; a = a->cdr) {
    if (a->car->type != TSTR)
      error("str: argument not a string");
    len += strlen(a->car->strv);
  }

  char ret[len + 1];
  char *last = &ret[0];

  // Append strings to return value
  for (Val *a = args; a != Nil; a = a->cdr) {
    last = stpcpy(last, a->car->strv);
  }

  ret[len + 1] = '\0';
  return make_str(root, &ret[0]);
}

// (str-len str)
static Val *prim_str_len(void *root, Val **env, Val **list) {
  DEFINE1(root, args);
  *args = eval_list(root, env, list);
  if (length(*args) != 1 || (*args)->car->type != TSTR) {
    error("str-len: 1st arg is not a string");
  }

  return make_int(root, strlen((*args)->car->strv));
}

// }}}

// {{{ primitives: math

// (+ <integer> ...)
static Val *prim_plus(void *root, Val **env, Val **list) {
  int sum = 0;
  for (Val *args = eval_list(root, env, list); args != Nil; args = args->cdr) {
    if (args->car->type != TINT)
      error("+ takes only numbers");
    sum += args->car->intv;
  }
  return make_int(root, sum);
}

// (- <integer> ...)
static Val *prim_minus(void *root, Val **env, Val **list) {
  Val *args = eval_list(root, env, list);
  for (Val *p = args; p != Nil; p = p->cdr)
    if (p->car->type != TINT)
      error("- takes only numbers");
  if (args->cdr == Nil)
    return make_int(root, -args->car->intv);
  int r = args->car->intv;
  for (Val *p = args->cdr; p != Nil; p = p->cdr)
    r -= p->car->intv;
  return make_int(root, r);
}

// (< <integer> <integer>)
static Val *prim_lt(void *root, Val **env, Val **list) {
  Val *args = eval_list(root, env, list);
  if (length(args) != 2)
    error("malformed <");
  Val *x = args->car;
  Val *y = args->cdr->car;
  if (x->type != TINT || y->type != TINT)
    error("< takes only numbers");
  return x->intv < y->intv ? True : Nil;
}

// (= <integer> <integer>)
static Val *prim_num_eq(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("Malformed =");
  Val *values = eval_list(root, env, list);
  Val *x = values->car;
  Val *y = values->cdr->car;
  if (x->type != TINT || y->type != TINT)
    error("= only takes numbers");
  return x->intv == y->intv ? True : Nil;
}

// (rand <integer>)
static Val *prim_rand(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("rand: takes exactly 1 argument");
  Val *values = eval_list(root, env, list);
  Val *x = values->car;
  if (x->type != TINT)
    error("rand: 1st arg is not an int");

  return make_int(root, pcg32_boundedrand(values->car->intv));
}

// }}}

// {{{ primitives: error

// (error message)
static Val *prim_error(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("error: takes exactly 1 argument");
  Val *values = eval_list(root, env, list);
  Val *str = values->car;
  if (str->type != TSTR)
    error("error: 1st arg is not a string");

  error(str->strv);
}

// (trap-error fn error-fn)
static Val *prim_trap_error(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("trap-error: takes exactly 2 arguments");
  Val *values = eval_list(root, env, list);

  DEFINE3(root, fn, error_fn, call);
  *fn = values->car;
  *error_fn = values->cdr->car;
  if ((*fn)->type != TFUN || (*error_fn)->type != TFUN)
    error("trap-error: both args must be functions");

  // check that we have space to save env
  if (error_depth >= MAX_ERROR_DEPTH) {
    fprintf(stderr,
            "Max error depth reached. Check for nested `trap-error` calls.\n");
    exit(1);
  }
  int trapped = setjmp(error_jmp_env[error_depth++]);
  if (trapped != 0) {
    *call = make_str(root, error_value);
    free(error_value);

    *call = cons(root, call, &Nil);
    *call = cons(root, error_fn, call);
  } else {
    *call = cons(root, fn, &Nil);
  }
  return eval(root, env, call);
}

// }}}

// {{{ primitives: os

// (write "str")
static Val *prim_write(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("write: not given exactly 2 args");

  Val *values = eval_list(root, env, list);

  if (values->car->type != TINT)
    error("write: 1st arg not file descriptor");
  if (values->cdr->car->type != TSTR)
    error("write: 2nd arg not string");

  int fd = values->car->intv;
  char *str = values->cdr->car->strv;

  if (write(fd, str, strlen(str)) < 0)
    error("write: error");
  return Nil;
}

// (read "str")
static Val *prim_read(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("read: not given exactly 2 args");

  Val *values = eval_list(root, env, list);

  if (values->car->type != TINT)
    error("read: 1st arg not file descriptor");
  if (values->cdr->car->type != TINT)
    error("read: 2nd arg not int");

  int fd = values->car->intv;
  int len = values->cdr->car->intv;

  char str[len + 1];
  bzero(str, len + 1);
  if (read(fd, &str, len) < 0)
    error("read: error");

  return make_str(root, str);
}

// (seconds)
static Val *prim_seconds(void *root, Val **env, Val **list) {
  (void)env;
  if (length(*list) != 0)
    error("seconds: takes no args");
  struct timespec spec;
  clock_gettime(CLOCK_REALTIME, &spec);
  return make_int(root, spec.tv_sec);
}

// (sleep n)
static Val *prim_sleep(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("sleep: not given exactly 1 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("sleep: 1st arg not int");

  int milliseconds = values->car->intv;
  struct timespec ts;
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds % 1000) * 1000000;
  nanosleep(&ts, NULL);
  return Nil;
}

// (exit code)
static Val *prim_exit(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("exit: not given exactly 1 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("exit: 1st arg not int");

  exit(values->car->intv);
  return Nil;
}

// (open path append-or-trunc) -> fd
static Val *prim_open(void *root, Val **env, Val **list) {
  if (length(*list) < 1)
    error("open: not given a path");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TSTR)
    error("open: 1st arg not string");

  // Check 2nd param (passed a mode to fopen(3))
  char *mode = "r";
  Val *rest = values->cdr;
  if (rest != Nil && rest->car->type == TSTR) {
    mode = rest->car->strv;
  }

  FILE *fd;
  if ((fd = fopen(values->car->strv, mode)) == NULL) {
    error("open: error opening file");
  }
  return make_int(root, fileno(fd));
}

// (close fd)
static Val *prim_close(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("close: not given exactly 1 arg");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("open: 1st arg not int");

  if (close(values->car->intv) < 0) {
    error("close: error closing file");
  }
  return Nil;
}

// (isatty fd)
static Val *prim_isatty(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("isatty: not given exactly 1 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("isatty: 1st arg not int");

  return isatty(values->car->intv) ? True : Nil;
}

// (getenv str)
static Val *prim_getenv(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("getenv: not given exactly 1 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TSTR)
    error("getenv: 1st arg not string");

  char *val = getenv(values->car->strv);
  if (val == NULL) {
    return Nil;
  }
  return make_str(root, val);
}

// }}}

// {{{ primitives: net

// (socket domain type protocol) -> fd
static Val *prim_socket(void *root, Val **env, Val **list) {
  if (length(*list) != 3)
    error("socket: not given exactly 3 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("socket: 1st arg not int");
  if (values->cdr->car->type != TINT)
    error("socket: 2nd arg not int");
  if (values->cdr->cdr->car->type != TINT)
    error("socket: 3rd arg not int");

  int domain = values->car->intv;
  int type = values->cdr->car->intv;
  int protocol = values->cdr->cdr->car->intv;

  int fd;
  if ((fd = socket(domain, type, protocol)) < 0) {
    error("socket: error creating socket");
  }

  if (setnonblock(fd) < 0) {
    error("socket: error making socket non-blocking");
  }

  return make_int(root, fd);
}

// (bind-inet socket-fd host port) -> fd
static Val *prim_bind_inet(void *root, Val **env, Val **list) {
  if (length(*list) != 3)
    error("bind-inet: not given exactly 3 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("bind-inet: 1st arg not int");
  if (values->cdr->car->type != TSTR)
    error("bind-inet: 2nd arg not string");
  if (values->cdr->cdr->car->type != TINT)
    error("bind-inet: 3rd arg not int");

  int socket_fd = values->car->intv;
  char *host = values->cdr->car->strv;
  int port = values->cdr->cdr->car->intv;

  struct sockaddr_in serv_addr;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  if (inet_aton(host, &serv_addr.sin_addr) < 0) {
    error("bind-inet: could not parse host");
  }
  if (bind(socket_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    error("bind-inet: error binding to address");
  }

  return Nil;
}

// (listen socket-fd backlog-size)
static Val *prim_listen(void *root, Val **env, Val **list) {
  if (length(*list) != 2)
    error("listen: not given exactly 2 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("listen: 1st arg not int");
  if (values->cdr->car->type != TINT)
    error("listen: 2nd arg not int");

  int socket_fd = values->car->intv;
  int backlog_size = values->cdr->car->intv;

  if (listen(socket_fd, backlog_size) < 0) {
    switch (errno) {
    case EACCES:
      error("listen: insuficient privileges");
    case EBADF:
      error("listen: given socket is not a valid file descriptor");
    case EINVAL:
      error("listen: socket is already listenning");
    case ENOTSOCK:
      error("listen: file descriptor given is not a valid socket");
    case EOPNOTSUPP:
      error("listen: socket type not supported");
    default:
      error("listen: error");
    }
  }

  return Nil;
}

// (accept socket-fd)
static Val *prim_accept(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("accept: not given exactly 1 args");
  Val *values = eval_list(root, env, list);
  if (values->car->type != TINT)
    error("accept: 1st arg not int");

  int client_fd;
  int socket_fd = values->car->intv;
  struct sockaddr_in c_addr;
  socklen_t c_addr_len = sizeof(c_addr);

  if ((client_fd = accept(socket_fd, (struct sockaddr *)&c_addr, &c_addr_len)) <
      0) {
    switch (errno) {
    case EINTR:
      return Nil; // accept interupted by a system call
    case EWOULDBLOCK:
      return Nil; // not ready to accept in a non-blocking way
    case EBADF:
      error("accept: given socket is not a valid file descriptor");
    case EINVAL:
      error("accept: socket is unwilling to accept connections");
    case ENOTSOCK:
      error("accept: file descriptor given is not a valid socket");
    case EOPNOTSUPP:
      error("accept: socket type is not SOCK_STREAM");
    case ENOMEM:
      error("accept: out of memory");
    case EMFILE:
      error("accept: process out of file descriptors");
    case ENFILE:
      error("accept: system out of file descriptors");
    default:
      error("accept: error");
    }
  }

  return make_int(root, client_fd);
}

// }}}

// {{{ primitives: ev

static void ev_io_watcher_callback(struct ev_loop *loop, ev_io *w,
                                   int revents) {
  (void)loop;
  (void)revents;
  WatcherState *wdata = w->data;
  void *root = NULL;
  DEFINE2(root, env, callback);
  *env = wdata->env;
  *callback = wdata->callback;
  apply_func(root, env, callback, &Nil);
}

static void ev_timer_watcher_callback(struct ev_loop *loop, ev_timer *w,
                                      int revents) {
  (void)loop;
  (void)revents;
  WatcherState *wdata = w->data;
  void *root = NULL;
  DEFINE2(root, env, callback);
  *env = wdata->env;
  *callback = wdata->callback;
  apply_func(root, env, callback, &Nil);
}

static void ev_signal_watcher_callback(struct ev_loop *loop, ev_signal *w,
                                       int revents) {
  (void)loop;
  (void)revents;
  WatcherState *wdata = w->data;
  void *root = NULL;
  DEFINE2(root, env, callback);
  *env = wdata->env;
  *callback = wdata->callback;
  apply_func(root, env, callback, &Nil);
}

// (ev-start type cb args*) -> int [wid]
static Val *prim_ev_start(void *root, Val **env, Val **list) {
  if (length(*list) < 2)
    error("ev-start: not given at least 2 argument");

  DEFINE4(root, values, type, cb, arg1);
  *values = eval_list(root, env, list);
  *type = (*values)->car;
  *cb = (*values)->cdr->car;
  if ((*type)->type != TINT)
    error("ev-start: type arg not an int");
  if ((*cb)->type != TFUN)
    error("ev-start: callback arg not a function");

#define ev_setup(T, ev_callback)                                               \
  T *w = malloc(sizeof(T));                                                    \
  ev_init(w, (ev_callback));                                                   \
  w->next = ev_watchers;                                                       \
  ev_watchers = (ev_watcher_list *)w;                                          \
  w->data = malloc(sizeof(WatcherState));                                      \
  WatcherState *wdata = (WatcherState *)w->data;                               \
  wdata->id = ev_next_id();                                                    \
  wdata->type = (*type)->intv;                                                 \
  wdata->env = *env;                                                           \
  wdata->callback = *cb;

  switch ((*type)->intv) {
  case EV_STAT:
    // TODO implement ev stat
    error("ev-start: TODO");
  case EV_READ:
  case EV_WRITE: {
    *arg1 = (*values)->cdr->cdr->car; // fd
    if ((*arg1)->type != TINT)
      error("ev-start: io watcher needs a file descriptor");

    ev_setup(ev_io, ev_io_watcher_callback);
    ev_io_set(w, (*arg1)->intv, wdata->type);
    ev_io_start(EV_DEFAULT_ w);

    return make_int(root, wdata->id);
  }
  case EV_TIMER: {
    *arg1 = (*values)->cdr->cdr->car; // delay
    if ((*arg1)->type != TINT)
      error("ev-start: timer watcher needs a delay as int");

    ev_setup(ev_timer, ev_timer_watcher_callback);
    double delay = (double)(*arg1)->intv / 1000.;
    ev_timer_set(w, delay, delay);
    ev_timer_start(EV_DEFAULT_ w);

    return make_int(root, wdata->id);
  }
  case EV_SIGNAL: {
    *arg1 = (*values)->cdr->cdr->car; // signal number
    if ((*arg1)->type != TINT)
      error("ev-start: signal watcher needs a signal number as integer");

    ev_setup(ev_signal, ev_signal_watcher_callback);
    ev_signal_set(w, (*arg1)->intv);
    ev_signal_start(EV_DEFAULT_ w);

    return make_int(root, wdata->id);
  }
  default:
    error("ev-start: unknown watcher type");
  }

#undef ev_setup
}

// (ev-stop wid) -> t|nil
static Val *prim_ev_stop(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("ev-stop: not given exactly 1 argument");

  DEFINE1(root, values);
  *values = eval_list(root, env, list);
  if ((*values)->car->type != TINT)
    error("ev-stop: 1st arg not int");

  ev_watcher_list *prevw = NULL;
  ev_watcher_list *w = ev_watchers;
  while (w != NULL) {
    WatcherState *wdata = w->data;
    if (wdata->id == (*values)->car->intv) {
      // Watcher found, stop, remove and free

      // Stop
      switch (wdata->type) {
      case EV_STAT:
        ev_stat_stop(EV_DEFAULT_(ev_stat *) w);
        break;
      case EV_READ:
      case EV_WRITE:
        ev_io_stop(EV_DEFAULT_(ev_io *) w);
        break;
      case EV_TIMER:
        ev_timer_stop(EV_DEFAULT_(ev_timer *) w);
        break;
      case EV_SIGNAL:
        ev_signal_stop(EV_DEFAULT_(ev_signal *) w);
        break;
      default:
        error("ev-stop: unknown watcher type");
      }

      // Remove from global watchers list
      if (prevw != NULL) {
        prevw->next = w->next;
      }

      // Free heap allocated watcher data
      free(wdata);
      free(w);
      return True;
    }

    prevw = w;
    w = w->next;
  }
  return Nil;
}

// }}}

// {{{ primitives: termios

static bool term_in_raw_mode = false;
static struct termios term_orig_attrs;

void term_disable_raw_mode() {
  if (term_in_raw_mode) {
    tcsetattr(STDIN_FILENO,TCSAFLUSH,&term_orig_attrs);
    term_in_raw_mode = false;
  }
}

int term_enable_raw_mode() {
    struct termios raw;

    if (term_in_raw_mode) return 0;
    if (!isatty(STDIN_FILENO)) goto fatal;
    atexit(term_disable_raw_mode);
    if (tcgetattr(STDIN_FILENO,&term_orig_attrs) == -1) goto fatal;

    raw = term_orig_attrs;
    // From kilo:
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer. */
    raw.c_cc[VMIN] = 0; /* Return each byte, or zero for timeout. */
    raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

    if (tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw) < 0) goto fatal;
    term_in_raw_mode = true;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static Val *prim_term_raw(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("term_raw: not given exactly 1 argument");
  DEFINE1(root, values);
  *values = eval_list(root, env, list);
  if ((*values)->car != Nil) {
    if (term_enable_raw_mode() < 0) {
      error("term_raw: error enabling raw mode");
    }
  } else {
    term_disable_raw_mode();
  }
  return Nil;
}

// }}}

// {{{ primitives: linenoise

// (linenoise prompt)
static Val *prim_linenoise(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("linenoise: not given exactly 1 argument");

  DEFINE2(root, values, str);
  *values = eval_list(root, env, list);
  if ((*values)->car->type != TSTR)
    error("linenoise: 1st arg not string");

  char *line = linenoise((*values)->car->strv);
  if (line == NULL) {
    return Nil;
  }
  *str = make_str(root, line);
  free(line);
  return *str;
}

// (linenoise-history-load path)
static Val *prim_linenoise_history_load(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("linenoise-history-load: not given exactly 1 argument");

  Val *values = eval_list(root, env, list);
  if (values->car->type != TSTR)
    error("linenoise-history-load: 1st arg not string");

  linenoiseHistoryLoad(values->car->strv);
  return Nil;
}

// (linenoise-history-add line)
static Val *prim_linenoise_history_add(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("linenoise-history-add: not given exactly 1 argument");

  Val *values = eval_list(root, env, list);
  if (values->car->type != TSTR)
    error("linenoise-history-add: 1st arg not string");

  linenoiseHistoryAdd(values->car->strv);
  return values->car;
}

// (linenoise-history-save path)
static Val *prim_linenoise_history_save(void *root, Val **env, Val **list) {
  if (length(*list) != 1)
    error("linenoise-history-save: not given exactly 1 argument");

  Val *values = eval_list(root, env, list);
  if (values->car->type != TSTR)
    error("linenoise-history-save: 1st arg not string");

  linenoiseHistorySave(values->car->strv);
  return Nil;
}

// }}}

static void add_primitive(void *root, Val **env, char *name, Primitive *fn) {
  DEFINE2(root, sym, prim);
  *sym = intern(root, name);
  *prim = make_primitive(root, fn);
  env_set(root, env, sym, prim);
}

static void define_constants(void *root, Val **env) {
  DEFINE2(root, sym, val);

  *sym = intern(root, "t");
  env_set(root, env, sym, &True);

  *sym = intern(root, "nil");
  env_set(root, env, sym, &Nil);

  *sym = intern(root, "*system-version*");
  *val = make_str(root, (char *)VERSION);
  env_set(root, env, sym, val);

#define defint(root, k, v)                                                     \
  *sym = intern(root, k);                                                      \
  *val = make_int(root, v);                                                    \
  env_set(root, env, sym, val);

  // Net
  defint(root, "PF_INET", PF_INET);
  defint(root, "SOCK_STREAM", SOCK_STREAM);

  // Ev
  defint(root, "EV_STAT", EV_STAT);
  defint(root, "EV_READ", EV_READ);
  defint(root, "EV_WRITE", EV_WRITE);
  defint(root, "EV_TIMER", EV_TIMER);
  defint(root, "EV_SIGNAL", EV_SIGNAL);

#undef defint
}

static void define_primitives(void *root, Val **env) {
  // Lists
  add_primitive(root, env, "cons", prim_cons);
  add_primitive(root, env, "car", prim_car);
  add_primitive(root, env, "cdr", prim_cdr);
  add_primitive(root, env, "set-car!", prim_set_car);

  // Strings
  add_primitive(root, env, "str", prim_str);
  add_primitive(root, env, "str-len", prim_str_len);

  // Language
  add_primitive(root, env, "def", prim_def);
  add_primitive(root, env, "def-global", prim_def_global);
  add_primitive(root, env, "set", prim_set);
  add_primitive(root, env, "fn", prim_fn);
  add_primitive(root, env, "if", prim_if);
  add_primitive(root, env, "do", prim_do);
  add_primitive(root, env, "while", prim_while);
  add_primitive(root, env, "eq?", prim_eq);
  add_primitive(root, env, "apply", prim_apply);
  add_primitive(root, env, "type", prim_type);
  add_primitive(root, env, "eval", prim_eval);
  add_primitive(root, env, "read-sexp", prim_read_sexp);
  add_primitive(root, env, "sym", prim_sym);

  // Macro
  add_primitive(root, env, "quote", prim_quote);
  add_primitive(root, env, "gensym", prim_gensym);
  add_primitive(root, env, "macro", prim_macro);
  add_primitive(root, env, "macro-expand", prim_macro_expand);

  // Object
  add_primitive(root, env, "obj", prim_obj);
  add_primitive(root, env, "obj-get", prim_obj_get);
  add_primitive(root, env, "obj-set", prim_obj_set);
  add_primitive(root, env, "obj-del", prim_obj_del);
  add_primitive(root, env, "obj-proto", prim_obj_proto);
  add_primitive(root, env, "obj-proto-set!", prim_obj_proto_set);
  add_primitive(root, env, "obj->alist", prim_obj_to_alist);

  // Math
  add_primitive(root, env, "+", prim_plus);
  add_primitive(root, env, "-", prim_minus);
  add_primitive(root, env, "<", prim_lt);
  add_primitive(root, env, "=", prim_num_eq);
  add_primitive(root, env, "rand", prim_rand);

  // Error
  add_primitive(root, env, "error", prim_error);
  add_primitive(root, env, "trap-error", prim_trap_error);

  // OS
  add_primitive(root, env, "pr-str", prim_pr_str);
  add_primitive(root, env, "write", prim_write);
  add_primitive(root, env, "read", prim_read);
  add_primitive(root, env, "seconds", prim_seconds);
  add_primitive(root, env, "sleep", prim_sleep);
  add_primitive(root, env, "exit", prim_exit);
  add_primitive(root, env, "open", prim_open);
  add_primitive(root, env, "close", prim_close);
  add_primitive(root, env, "isatty", prim_isatty);
  add_primitive(root, env, "getenv", prim_getenv);

  // Net
  add_primitive(root, env, "socket", prim_socket);
  add_primitive(root, env, "bind-inet", prim_bind_inet);
  add_primitive(root, env, "listen", prim_listen);
  add_primitive(root, env, "accept", prim_accept);

  // Ev
  add_primitive(root, env, "ev-start", prim_ev_start);
  add_primitive(root, env, "ev-stop", prim_ev_stop);

  // Term
  add_primitive(root, env, "term-raw", prim_term_raw);

  // Linenoise
  add_primitive(root, env, "linenoise", prim_linenoise);
  add_primitive(root, env, "linenoise-history-load",
                prim_linenoise_history_load);
  add_primitive(root, env, "linenoise-history-add", prim_linenoise_history_add);
  add_primitive(root, env, "linenoise-history-save",
                prim_linenoise_history_save);
}
// }}}

// {{{ main

// Returns true if the environment variable is defined and not the empty string.
static bool get_env_flag(char *name) {
  char *val = getenv(name);
  return val && val[0];
}

// Ran right after the event loop is start so that evaluated code in here runs
// in the context
// of a working event loop.
// Call the `shi-main` method from the prelude at the end.
static void shi_init_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  (void)revents;
  ev_timer_stop(loop, w);

  // Defining `root` is dangerous as the next GC will wipe all that already on
  // the stack from previous `root`s. But, since this is our entrypoint and
  // that we have no good ways of relaying root to shi_init_cb, it's safe.
  void *root = NULL;
  DEFINE4(root, env, shi_main, read_sexp_sym, prelude);
  *env = (Val *)w->data;

  // Read and evaluate prelude
  *read_sexp_sym = intern(root, "read-sexp");
  *prelude = make_str(root, (char *)prelude_contents);
  *prelude = cons(root, prelude, &Nil);
  *prelude = cons(root, read_sexp_sym, prelude);
  *prelude = eval(root, env, prelude);
  eval(root, env, prelude);

  *shi_main = intern(root, "shi-main");
  *shi_main = cons(root, shi_main, &Nil);
  eval(root, env, shi_main);
}

int main(int argc, char **argv) {
  // Seed random number generator
  pcg32_srandom(time(NULL) ^ (intptr_t)&printf, (intptr_t)&gc);

  // Debug flags
  debug_gc = get_env_flag("SHI_DEBUG_GC");
  always_gc = get_env_flag("SHI_ALWAYS_GC");

  // Memory allocation
  memory = alloc_semispace();

  // Constants and primitives
  symbols = Nil;
  void *root = NULL;
  DEFINE4(root, env, sh_args_sym, sh_args, sh_arg);
  *env = make_obj_alist(root, &Nil, &Nil);
  define_constants(root, env);
  define_primitives(root, env);

  // Register shell args in env
  *sh_args_sym = intern(root, "*args*");
  *sh_args = Nil;
  for (int i = 0; i < argc; i++) {
    *sh_arg = make_str(root, argv[i]);
    *sh_args = cons(root, sh_arg, sh_args);
  }
  *sh_args = reverse(*sh_args);
  env_set(root, env, sh_args_sym, sh_args);

  // Start event loop
  ev_timer *shi_init_w = malloc(sizeof(ev_timer));
  ev_timer_init(shi_init_w, shi_init_cb, 0., 0.);
  shi_init_w->data = *env;
  ev_timer_start(EV_DEFAULT_ shi_init_w);
  ev_run(EV_DEFAULT_ 0);
  return 0;
}

// }}}
