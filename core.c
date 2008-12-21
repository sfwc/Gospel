/*
    Copyright 2008 Sam Chapin

    This file is part of Gospel.

    Gospel is free software: you can redistribute it and/or modify
    it under the terms of version 3 of the GNU General Public License
    as published by the Free Software Foundation.

    Gospel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Gospel.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

// Used by socket objects:
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "death.h"
#include "core.h"
#include "gc.h"
#include "objects.h"
#include "threadData.h"
#include "parser.h"

#define NIL 0

int empty(pair l) {
  return l == emptyList;
}

pair list(vector *live, void *o) {
  return cons(live, o, emptyList);
}

pair nreverse(pair l) {
  pair next, last = emptyList;
  while (!empty(l)) {
    next = cdr(l);
    setcdr(l, last);
    last = l;
    l = next;
  }
  return last;
}

int length(pair l) {
  int i = 0;
  while (!empty(l)) {
    l = cdr(l);
    i++;
  }
  return i;
}

vector listToVector(vector *live, vector list) {
  vector living = newVector(live, 2, list, 0);
  vector v = setIdx(living, 1, makeVector(edenIdx(living, 1), length(list)));
  for (int i = 0; !empty(list); i++, list = cdr(list)) setIdx(v, i, car(list));
  return *live = v;
}

void *fold(void *op, pair args, void *identity) {
  return empty(args) ? identity
                     : ((void *(*)(void *, void *))op)(car(args), fold(op, cdr(args), identity));
}
pair map(life live, void *fn, pair list) {
  return empty(list) ? list
                     : cons(live, ((void *(*)(void *))fn)(car(list)), map(live, fn, cdr(list)));
}

typedef vector continuation;

continuation newContinuation(vector *live,
                             void *origin,
                             obj selector,
                             vector evaluated,
                             vector unevaluated,
                             vector staticEnv,
                             vector dynamicEnv) {
  return newVector(live, 7, origin, selector, evaluated, unevaluated, staticEnv, dynamicEnv, 0);
}

obj receiver(continuation c) {
  return idx(c, 6);
}
vector setThreadReceiver(vector thread, obj o) {
  setIdx(threadContinuation(thread), 6, o);
  return thread;
}

void   *origin(continuation c)      { return idx(c, 0); } // either a promise or another continuation
obj     selector(continuation c)    { return idx(c, 1); }
vector  evaluated(continuation c)   { return idx(c, 2); }
vector  unevaluated(continuation c) { return idx(c, 3); }
obj     env(continuation c)         { return idx(c, 4); }
obj     dynamicEnv(continuation c)  { return idx(c, 5); }

void setEvaluated(continuation c, vector v)   { setIdx(c, 2, v); }
void setUnevaluated(continuation c, vector v) { setIdx(c, 3, v); }
void setEnv(continuation c, obj o)            { setIdx(c, 4, o); }

int isVisible(continuation c, obj namespace) {
  vector namespaces = hiddenEntity(dynamicEnv(c));
  for (int i = 0; i < vectorLength(namespaces); ++i)
    if (idx(namespaces, i) == namespace) return -1;
  return 0;
}
obj currentNamespace(continuation c) {
  return idx(hiddenEntity(dynamicEnv(c)), 0);
}

obj continuationTarget(continuation c) {
  return waitFor(idx(evaluated(c), 0));
}

vector setSubexpressionContinuation(vector thread,
                                    continuation parentContinuation,
                                    obj staticEnv,
                                    obj dynamicEnv,
                                    obj target,
                                    obj selector,
                                    vector args) {
  vector eden = newVector(edenRoot(thread), 2, *edenRoot(thread), 0);
  return setContinuation(thread,
                         newContinuation(edenRoot(thread),
                                         parentContinuation,
                                         selector,
                                         emptyVector, // initially no subexpressions evaluated
                                         prefix(edenIdx(eden, 1), target, args),
                                         staticEnv,
                                         dynamicEnv));
}

vector setMessageReturnContinuation(vector thread, continuation c, obj value) {
  return setContinuation(thread,
                         newContinuation(edenRoot(thread),
                                         origin(c),
                                         selector(c),
                                         suffix(edenRoot(thread), value, evaluated(c)),
                                         unevaluated(c),
                                         env(c),
                                         dynamicEnv(c)));
}

#define gotoNext(thread) tailcall(doNext, (thread))

#define messageReturn(thread, value) do { \
  vector mr_t = (thread), mr_v = (value); \
  continuation mr_c = origin(threadContinuation(mr_t)); \
  /* This logic can't easily be moved from the macro into the function call,
     because the return from keep() is what kills the thread. */ \
  if (isPromise(mr_c)) tailcall(keep, mr_t, mr_c, mr_v); \
  gotoNext(setMessageReturnContinuation(mr_t, mr_c, mr_v)); \
} while (0)

vector setExceptionContinuation(vector thread, obj exception) {
  continuation c = threadContinuation(thread);
  return setSubexpressionContinuation(thread,
                                      origin(c),
                                      env(c),
                                      dynamicEnv(c),
                                      env(c),
                                      sRaise_,
                                      newVector(edenRoot(thread), 1, exception));
}

#define raise(thread, exception) \
  gotoNext(setExceptionContinuation((thread), (exception)))

obj string(vector *live, const char *s) {
  int length = CELLS_REQUIRED_FOR_BYTES(strlen(s) + 1);
  strcpy((char *)makeAtomVector(live, length)->data, s);
  return slotlessObject(live, oString, *live);
}
char *stringData(obj s) {
  return (char *)(hiddenEntity(s)->data);
}
int stringLength(obj s) {
  vector data = hiddenEntity(s);
  int i = vectorLength(data);
  if (!i) return 0;
  int length = i * 4;
  char *last = (char *)idxPointer(data, i - 1);
  return !last[0] ? length - 4
       : !last[1] ? length - 3
       : !last[2] ? length - 2
                  : length - 1;
}

obj appendStrings(vector *live, obj s1, obj s2) {
  vector living = newVector(live, 3, s1, s2, 0);
  int length1 = stringLength(s1);
  vector s = makeAtomVector(edenIdx(living, 2), CELLS_REQUIRED_FOR_BYTES(length1 + stringLength(s2) + 1));
  strcpy((char *)vectorData(s), stringData(s1));
  strcpy((char *)vectorData(s) + length1, stringData(s2));
  return slotlessObject(live, oString, s);
}

obj block(vector *live, vector params, vector body) {
  return slotlessObject(live, oBlock, newVector(live, 2, params, body));
}
vector blockParams(obj b) { return idx(hiddenEntity(b), 0); }
vector blockBody(obj b)   { return idx(hiddenEntity(b), 1); }

vector vectorAppend(vector *live, vector v1, vector v2) {
  int length1 = vectorLength(v1), length2 = vectorLength(v2);
  vector living = newVector(live, 3, v1, v2, 0),
         v3 = makeVector(edenIdx(living, 2), length1 + length2);
  memcpy(vectorData(v3), vectorData(v1), length1 * sizeof(void *));
  memcpy((void *)vectorData(v3) + length1 * sizeof(void *), vectorData(v2), length2 * sizeof(void *));
  return v3;
}

void **shallowLookup(obj o, obj name, vector c) {
  for (int i = 0; i < slotCount(o); ++i)
    if (slotName(o, i) == name && isVisible(c, slotNamespace(o, i)))
      return slotValuePointer(o, i);
  return 0;
}
void **deepLookup(obj o, obj name, continuation c) {
  void **slot;
  if (slot = shallowLookup(o, name, c)) return slot;
  return o == oNull ? 0 : deepLookup(proto(o), name, c);
}

obj addSlot(life e, obj o, obj s, void *v, continuation c) {
  void **slot = shallowLookup(o, s, c);
  return slot ? *slot = v : newSlot(e, o, s, v, currentNamespace(c));
}

obj vectorObject(vector *eden, vector v) {
  return slotlessObject(eden, oVector, v);
}
obj vectorObjectVector(obj v) {
  return hiddenEntity(v);
}

vector vectorUnion(life e, vector x, vector y) {
  int nx = vectorLength(x), n = nx, ny = vectorLength(y);
  obj *t = malloc((nx + ny) * sizeof(obj));
  memcpy(t, vectorData(x), nx * sizeof(obj));
  for (int i = 0; i < ny; ++i) {
    for (int j = 0; j < n; ++j) if (idx(y, i) == t[j]) goto alreadyPresent;
    t[n++] = idx(y, i);
    continue;
    alreadyPresent:;
  }
  obj v = makeVector(e, n);
  memcpy(vectorData(v), t, n * sizeof(obj));
  free(t);
  return v;
}

void doNext(vector);
vector newThread(vector *life,
                 void *cc,
                 obj staticEnv,
                 obj dynamicEnv,
                 obj target,
                 obj selector,
                 vector args) {
  vector living = newVector(life, 6, cc, staticEnv, dynamicEnv, target, selector, args),
         threadData = addThread(edenIdx(living, 1), garbageCollectorRoot),
         *newLife = edenIdx(living, 3);
  setContinuation(threadData,
                  newContinuation(newLife,
                                  cc,
                                  selector,
                                  emptyVector,
                                  prefix(newLife, target, args),
                                  staticEnv,
                                  dynamicEnv));
  spawn(topOfStack(threadData), doNext, threadData);
  return threadData;
}

promise REPLPromise;
void returnToREPL(vector thread) {
  keep(thread, REPLPromise, oNull);
}
#define abort(thread, ...) do { \
  printf(__VA_ARGS__); \
  fflush(stdout); \
  tailcall(returnToREPL, (thread)); \
} while (0)

void invokeDispatchMethod(vector);

obj newDynamicScope(life e, continuation c) {
  obj oldScope = dynamicEnv(c);
  return slotlessObject(e, oldScope, hiddenEntity(oldScope));
}

void normalDispatchMethod(vector thread) {
  vector c = threadContinuation(thread);
  obj r = receiver(c);
  void **slot = shallowLookup(r, selector(c), c);
  if (!slot) tailcall(invokeDispatchMethod, setThreadReceiver(thread, proto(r))); 
  obj contents = *slot;

  if (isPrimitive(contents)) tailcall(primitiveCode(contents), thread);
  if (isClosure(contents)) {
    vector params = closureParams(contents), args = evaluated(c);
    if (vectorLength(args) != vectorLength(params)) raise(thread, eBadArity);
    vector eden = makeVector(edenRoot(thread), 2);
    gotoNext(setSubexpressionContinuation(thread,
                                          origin(c),
                                          stackFrame(edenIdx(eden, 0),
                                                     closureEnv(contents),
                                                     params,
                                                     args,
                                                     c),
                                          newDynamicScope(edenIdx(eden, 1), c),
                                          oInternals,
                                          sMethodBody,
                                          closureBody(contents)));
  }
  messageReturn(thread, contents); // The slot contains a constant value, not code.
}

void invokeDispatchMethod(vector thread) {
  continuation c = threadContinuation(thread);
  obj dm = dispatchMethod(receiver(c));

  // TODO: Find a nice solution to the bootstrapping problem of initializing all objects with a
  //       primitive method object containing normalDispatchMethod, and eliminate the first test.

  if (!dm) tailcall(normalDispatchMethod, thread);
  if (isPrimitive(dm)) tailcall(primitiveCode(dm), thread);
  if (isClosure(dm)) {
    // Closures are checked for correct arity at the time the dispatch method is set.
    vector eden = makeVector(edenRoot(thread), 2);
    gotoNext(setSubexpressionContinuation(thread,
                                          origin(c),
                                          stackFrame(edenIdx(eden, 0),
                                                     closureEnv(dm),
                                                     closureParams(dm),
                                                     evaluated(c),
                                                     c),
                                          slotlessObject(edenIdx(eden, 1), dynamicEnv(c), NIL),
                                          oInternals,
                                          sMethodBody,
                                          closureBody(dm)));
  }
  messageReturn(thread, dm); // The dispatch method is actually just a constant value.
}

void dispatch(vector thread) {
  continuation c = threadContinuation(thread);
  obj t = continuationTarget(c);
  if (isChannel(t)) {
    vector e = makeVector(edenRoot(thread), 3);
    promise p = newPromise(edenIdx(e, 0));
    acquireChannelLock(t);
    vector behindChannel = duplicateVector(edenIdx(e, 1), evaluated(c));
    setIdx(behindChannel, 0, channelTarget(t));

    vector channelThread = addThread(edenIdx(e, 2), garbageCollectorRoot);
    setContinuation(channelThread,
                    newContinuation(edenIdx(e, 2),
                                    p,
                                    selector(c),
                                    behindChannel,
                                    behindChannel,
                                    env(c),
                                    dynamicEnv(c)));
    spawn(topOfStack(channelThread), dispatch, channelThread);

    obj r = waitFor(p);
    releaseChannelLock(t);
    messageReturn(thread, r);
  }
  tailcall(invokeDispatchMethod, setThreadReceiver(thread, t));
}

// The heart of the interpreter, called at the end of each expression to evaluate the next one.
void doNext(vector thread) {
  setShelter(thread, 0); // Release temporary allocations from the last subexpression.
  continuation c = threadContinuation(thread);
  int evaluatedCount = vectorLength(evaluated(c)),
      unevaluatedCount = vectorLength(unevaluated(c));
  // Have we evaluated all the subexpressions?
  if (evaluatedCount == unevaluatedCount) tailcall(dispatch, thread);
  // If not, evaluate the next subexpression.
  vector *live = edenRoot(thread),
         subexpr = newVector(live, 1, idx(unevaluated(c), evaluatedCount));
  tailcall(dispatch,
           setContinuation(thread,
                           newContinuation(live,
                                           c,
                                           sInterpret,
                                           subexpr,
                                           subexpr,
                                           env(c),
                                           dynamicEnv(c))));
}

obj call(vector *live, obj dynamicEnv, obj target, obj selector, vector args) {
  promise p = newPromise(live);
  newThread(live, p, oLobby, dynamicEnv, target, selector, args);
  return waitFor(p);
}

vector cons(vector *live, void *car, void *cdr) {
  return newVector(live, 2, car, cdr);
}
void *car(vector pair) { return idx(pair, 0); }
void *cdr(vector pair) { return idx(pair, 1); }

vector setcar(vector pair, void *val) { setIdx(pair, 0, val); }
vector setcdr(vector pair, void *val) { setIdx(pair, 1, val); }

int isEmpty(vector v) {
  return vectorLength(v) == 0; 
}

obj intern(vector *live, obj symbol) {
  acquireSymbolTableLock();
  obj symbolTable = *symbolTableShelter(garbageCollectorRoot);
  char *string = stringData(symbol);
  for (pair st = symbolTable; !empty(st); st = cdr(st))
    if (!strcmp(string, stringData(car(st)))) {
      releaseSymbolTableLock();
      return car(st);
    }
  *symbolTableShelter(garbageCollectorRoot) = cons(live, symbol, symbolTable);
  releaseSymbolTableLock();
  return symbol;
}

obj  codeTarget(obj c)   { return idx(hiddenEntity(c), 0); }
obj  codeSelector(obj c) { return idx(hiddenEntity(c), 1); }
pair codeArgs(obj c)     { return idx(hiddenEntity(c), 2); }

obj message(vector *live, obj target, obj selector, vector args) {
  return slotlessObject(live, oCode, newVector(live, 3, target, selector, args));
}
obj expressionSequence(vector *live, vector exprs) {
  return message(live, oInternals, sMethodBody, exprs);
}
obj promiseCode(vector *live, obj message) {
  return slotlessObject(live, oPromiseCode, message);
}
obj promiseCodeValue(obj p) {
  return hiddenEntity(p);
}

vector *temp() {
  acquireTempLock();
  vector *s = edenRoot(garbageCollectorRoot);
  vector v = newVector(s, 2, 0, *s);
  releaseTempLock();
  return idxPointer(v, 0);
}
void invalidateTemporaryLife() {
  setShelter(garbageCollectorRoot, 0);
}

obj threadTarget(vector td) { return continuationTarget(threadContinuation(td)); }

void *safeIdx(vector thread, vector v, int i) {
  return i < vectorLength(v) ? idx(v, i) : 0;
}
void *arg(vector thread, int i) {
  return safeIdx(thread, evaluated(threadContinuation(thread)), i + 1);
}

obj *filenameToInclude;
promise *promiseOfInclusion;

// The prototype primitive's behaviour should be to return itself, so that it won't be necessary to write
// e.g. "\primitive foo {}".
void prototypePrimitiveHiddenValue(vector thread) {
  messageReturn(thread, oPrimitive);
}

void *loadStream(life, FILE *, obj, obj);

#define safeIntegerValue(i) ({ \
  obj s_i = (i); \
  isInteger(s_i) ? integerValue(s_i) : ({ raise(thread, eIntegerExpected); 0; }); \
})
#define safeStackFrameContinuation(f) ({ \
  obj s_f = (f); \
  isStackFrame(s_f) ? stackFrameContinuation(s_f) : ({ raise(thread, eStackFrameExpected); \
                                                       (vector)0; \
                                                     }); \
})
#define safeStringValue(s) ({ \
  obj s_s = (s); \
  isString(s_s) ? stringData(s_s) : ({ raise(thread, eStringExpected); (char *)0; }); \
})
#define safeVector(v) ({ \
  obj s_v = (v); \
  isVectorObject(s_v) ? vectorObjectVector(s_v) : ({ raise(thread, eVectorExpected); (vector)0; }); \
}) 
#define normalReturn messageReturn(thread, continuationTarget(threadContinuation(thread)))
#define valueReturn(v) messageReturn(thread, (v))
#define arg(i) (arg(thread, (i)) ?: ({ raise(thread, eMissingArgument); (void *)0; }))
#define target continuationTarget(threadContinuation(thread))
#include "objects.c"
#undef target
#undef arg
#undef valueReturn
#undef normalReturn
#undef safeVector
#undef safeStringValue
#undef safeStackFrameContinuation
#undef safeIntegerValue

void (*primitiveCode(obj p))(continuation);

obj appendSymbols(life eden, pair symbols) {
  vector e = newVector(eden, 2, symbols, 0);
  obj s = string(edenIdx(e, 1), "");
  for (; !empty(symbols); symbols = cdr(symbols))
    s = appendStrings(edenIdx(e, 1), s, car(symbols));
  setProto(s, oSymbol);
  return intern(eden, s);
}

obj symbol(life eden, const char *s) {
  obj o = string(eden, s);
  setProto(o, oSymbol);
  return intern(eden, o);
}

void setupInterpreter() {
  initializeHeap();
  vector dummyEden;
  garbageCollectorRoot = createGarbageCollectorRoot(&dummyEden);
  initializeObjects();
  initializePrototypeTags();
  *lobbyShelter(garbageCollectorRoot) = oLobby;
  intern(temp(), oSymbol);
}

void *loadStream(life eden, FILE *f, obj staticEnv, obj dynamicEnv) {
  void *scanner = beginParsing(f);
  obj parserOutput, lastValue = oNull;
  vector e = makeVector(eden, 2);
  while (parserOutput = parse(scanner)) {
    promise p = newPromise(edenIdx(e, 0));
    newThread(edenIdx(e, 1), p, staticEnv, dynamicEnv, parserOutput, sIdentity, emptyVector);
    lastValue = waitFor(p);
  }
  endParsing(scanner);
  fclose(f);
  return lastValue;
}
void *loadFile(vector *eden, const char *filename, obj staticEnv, obj dynamicEnv) {
  FILE *f = fopen(filename, "r");
  if (!f) die("Could not open \"%s\" for input.", filename);
  return loadStream(eden, f, staticEnv, dynamicEnv);
}

void REPL() {
  void *scanner = beginParsing(stdin);
  for (;;) {
    invalidateTemporaryLife();
    fputs("\n> ", stdout);
    fflush(stdout);
    // We just serialize first, to give the expression a chance to do its own terminal output, before
    // displaying the "=>" and printing.
    REPLPromise = newPromise(temp());
    obj parserOutput = parse(scanner);
    if (!parserOutput) exit(0); // EOF character was input.
    newThread(temp(), REPLPromise, oLobby, oDynamicEnvironment, parserOutput, sSerialized, emptyVector);
    obj serialization = waitFor(REPLPromise);
    fputs("\n=> ", stdout);
    fflush(stdout);
    REPLPromise = newPromise(temp());
    newThread(temp(), REPLPromise, oLobby, oDynamicEnvironment, serialization, sPrint, emptyVector);
    waitFor(REPLPromise);
  }
}
