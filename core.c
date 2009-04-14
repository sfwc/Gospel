/*
    Copyright © 2008, 2009 Sam Chapin

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

// Used for bignum arithmetic.
#include <gmp.h>

#include <regex.h>

// Used by socket objects:
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

// Used by fileStream objects. 
#include <sys/stat.h>
#include <fcntl.h>


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

pair list(void *o) {
  return cons(o, emptyList);
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

pair nappend(pair x, pair y) {
  pair first = x, next;
  while (!empty(next = cdr(x))) x = next;
  setcdr(x, y);
  return first;
}

int length(pair l) {
  int i = 0;
  while (!empty(l)) {
    l = cdr(l);
    i++;
  }
  return i;
}

vector listToVector(vector list) {
  vector v = makeVector(length(list));
  for (int i = 0; !empty(list); i++, list = cdr(list)) setIdx(v, i, car(list));
  return v;
}

pair map(void *fn, pair list) {
  return empty(list) ? list : cons(((void *(*)(void *))fn)(car(list)), map(fn, cdr(list)));
}

typedef vector continuation;

continuation newContinuation(void *origin,
                             obj selector,
                             vector evaluated,
                             vector unevaluated,
                             vector staticEnv,
                             vector dynamicEnv) {
  return newVector(7, origin, selector, evaluated, unevaluated, staticEnv, dynamicEnv, 0);
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
  return setContinuation(thread,
                         newContinuation(parentContinuation,
                                         selector,
                                         emptyVector, // initially no subexpressions evaluated
                                         prefix(target, args),
                                         staticEnv,
                                         dynamicEnv));
}

vector prepareMessageReturn(obj value) {
  continuation c = origin(threadContinuation(currentThread));
  if (isPromise(c)) {
    keep(currentThread, c, value);
    explicitlyEndThread();
  }  
  return setContinuation(currentThread,
                         newContinuation(origin(c),
                                         selector(c),
                                         suffix(value, evaluated(c)),
                                         unevaluated(c),
                                         env(c),
                                         dynamicEnv(c)));
}

#ifdef NO_COMPUTED_TAILCALLS
  #define gotoNext return
  #define computeTailcall(ct_e) do { (ct_e)(); tailcall(doNext); } while (0)
#else
  #define gotoNext tailcall(doNext)
  #define computeTailcall(ct_e) tailcall(ct_e)
#endif

// messageReturn() is equivalent to staticMessageReturn() if compiling with support for tailcalls to
// function pointers, but otherwise performs the C function return that primitive-calling code will in
// that event be expecting.
#define messageReturn(mr_v) do { prepareMessageReturn(mr_v); gotoNext; } while (0)
#define staticMessageReturn(smr_v) do { prepareMessageReturn(smr_v); tailcall(doNext); } while (0)


vector setExceptionContinuationWithEnvironment(vector thread, obj exception, obj environment) {
  continuation c = threadContinuation(thread);
  return setSubexpressionContinuation(thread,
                                      origin(c),
                                      env(c),
                                      environment,
                                      exception,
                                      sRaise,
                                      emptyVector);
}
vector setExceptionContinuation(vector thread, obj exception) {
  return setExceptionContinuationWithEnvironment(thread,
                                                 exception,
                                                 dynamicEnv(threadContinuation(thread)));
}

#define raise(raise_exception) do { \
  setExceptionContinuation(currentThread, (raise_exception)); \
  gotoNext; \
} while (0)

obj appendStrings(obj s1, obj s2) {
  int length1 = stringLength(s1);
  vector s = makeAtomVector(CELLS_REQUIRED_FOR_BYTES(length1 + stringLength(s2) + 1));
  strcpy((char *)vectorData(s), stringData(s1));
  strcpy((char *)vectorData(s) + length1, stringData(s2));
  return slotlessObject(oString, s);
}

obj blockLiteral(vector params, vector body) {
  return slotlessObject(oBlockLiteral, method(params, body));
}
int isBlockLiteral(obj bl) { return isMethod(hiddenEntity(bl)); }

int methodArity(obj m) {
  return vectorLength(methodParams(m)) - 1;
}

obj block(obj env, obj method) {
  return slotlessObject(oBlock, newVector(2, env, method));
}
obj blockEnv(obj b)    { return idx(hiddenEntity(b), 0); }
obj blockMethod(obj b) { return idx(hiddenEntity(b), 1); }
obj setBlockEnv(obj b, obj e) { return setIdx(hiddenEntity(b), 0, e); }

int isBlock(obj b) { // FIXME: This is not a perfect test.
  return vectorLength(hiddenEntity(b)) == 2 && isMethod(blockMethod(b));
}

vector vectorAppend(vector v1, vector v2) {
  int length1 = vectorLength(v1), length2 = vectorLength(v2);
  vector v3 = makeVector(length1 + length2);
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

obj addCanonSlot(obj o, obj s, void *v) {
  return newSlot(o, s, v, oNamespaceCanon);
}
obj addSlot(obj o, obj s, void *v, continuation c) {
  void **slot = shallowLookup(o, s, c);
  return slot ? *slot = v : newSlot(o, s, v, currentNamespace(c));
}

void doNext(void);
vector newThread(void *cc,
                 obj staticEnv,
                 obj dynamicEnv,
                 obj target,
                 obj selector,
                 vector args) {
  vector threadData = addThread(garbageCollectorRoot);
  
  setContinuation(threadData,
                  newContinuation(cc,
                                  selector,
                                  emptyVector,
                                  prefix(target, args),
                                  staticEnv,
                                  dynamicEnv));
  spawn(doNext, threadData);
  return threadData;
}

void invokeDispatchMethod(void);

obj newEnvironment(obj oldEnv) {
  return typedObject(oldEnv, hiddenEntity(oldEnv));
}

void setMethodContinuation(vector c, obj scope, vector args, obj method) {
  setSubexpressionContinuation(currentThread,
                               origin(c),
                               stackFrame(scope, methodParams(method), args, c),
                               newEnvironment(dynamicEnv(c)),
                               oMethodBody,
                               sSubexpressions_,
                               methodBody(method));
}

void normalDispatchMethod() {
  vector c = threadContinuation(currentThread);
  obj r = receiver(c);
  void **slot = shallowLookup(r, selector(c), c);
  if (!slot) {
    setThreadReceiver(currentThread, proto(r));
    tailcall(invokeDispatchMethod);
  }
  obj contents = *slot;

  if (isPrimitive(contents)) computeTailcall(primitiveCode(contents));
  if (isMethod(contents)) {
    vector params = methodParams(contents), args = evaluated(c);
    if (vectorLength(args) != vectorLength(params)) {
      setExceptionContinuation(currentThread, eBadArity);
      tailcall(doNext);
    }
    setMethodContinuation(c, continuationTarget(c), args, contents);
    tailcall(doNext);
  }
  staticMessageReturn(contents); // The slot contains a constant value, not code.
}

void invokeDispatchMethod() {
  continuation c = threadContinuation(currentThread);
  obj dm = dispatchMethod(receiver(c));

  // TODO: Find a nice solution to the bootstrapping problem of initializing all objects with a
  //       primitive method object containing normalDispatchMethod, and eliminate the first test.

  if (!dm) tailcall(normalDispatchMethod);
  if (isPrimitive(dm)) computeTailcall(primitiveCode(dm));
  if (isMethod(dm)) {
    // Method is checked for correct arity when it's installed.
    setMethodContinuation(c, continuationTarget(c), evaluated(c), dm);
    tailcall(doNext);
  }
  staticMessageReturn(dm); // The dispatch method is actually just a constant value.
}

void dispatch() {
  continuation c = threadContinuation(currentThread);
  obj t = continuationTarget(c);
  if (isActor(t)) {
    vector evaled = evaluated(c);
    int n = vectorLength(evaled) - 1;
    vector args = makeVector(n);
    for (int i = 0; i < n; ++i) setIdx(args, i, idx(evaled, i + 1));
    staticMessageReturn(enqueueMessage(t, selector(c), args));
  }
  setThreadReceiver(currentThread, t);
  tailcall(invokeDispatchMethod);
}

// The heart of the interpreter, called at the end of each expression to evaluate the next one.
void doNext() {
  shelter(currentThread, 0); // Release temporary allocations from the last subexpression.
  continuation c = threadContinuation(currentThread);
  int evaluatedCount = vectorLength(evaluated(c)),
      unevaluatedCount = vectorLength(unevaluated(c));
  // Have we evaluated all the subexpressions?
  if (evaluatedCount >= unevaluatedCount) tailcall(dispatch);
  // If not, evaluate the next subexpression.
  vector subexpr = newVector(1, idx(unevaluated(c), evaluatedCount));
  setContinuation(currentThread,
                  newContinuation(c,
                                  sInterpret,
                                  subexpr,
                                  subexpr,
                                  env(c),
                                  dynamicEnv(c)));
  tailcall(dispatch);
}

obj callWithEnvironment(obj env, obj target, obj selector, vector args) {
  promise p = newPromise();
  obj newEnv = newEnvironment(env);
  // FIXME: Assumes that the Canon namespace will always be visible, which is probably a bad idea.
  addCanonSlot(newEnv,
               sHandler,
               block(oNull,
                     method(newVector(2, sCurrentMessageTarget, oSymbol),
                            newVector(1,
                                      message(oInternals,
                                              sTerminateThread_andReraise_in_,
                                              newVector(3,
                                                        quote(p),
                                                        message(oDefaultMessageTarget,
                                                                oSymbol,
                                                                emptyVector),
                                                        quote(env)))))));
  newThread(p, oObject, newEnv, target, selector, args);
  obj v = waitFor(p);
  if (v == oThreadExpiryIndicator) explicitlyEndThread();
  return v;
}
obj call(obj target, obj selector, vector args) {
  return callWithEnvironment(dynamicEnv(threadContinuation(currentThread)), target, selector, args);
}

vector cons(void *car, void *cdr) {
  return newVector(2, car, cdr);
}
void *car(vector pair) { return idx(pair, 0); }
void *cdr(vector pair) { return idx(pair, 1); }

vector setcar(vector pair, void *val) { return setIdx(pair, 0, val); }
vector setcdr(vector pair, void *val) { return setIdx(pair, 1, val); }

int isEmpty(vector v) {
  return vectorLength(v) == 0; 
}

obj intern(obj symbol) {
  acquireSymbolTableLock();
  obj symbolTable = hiddenEntity(oInternals);
  char *string = stringData(symbol);
  for (pair st = symbolTable; !empty(st); st = cdr(st))
    if (!strcmp(string, stringData(car(st)))) {
      releaseSymbolTableLock();
      return car(st);
    }
  setHiddenData(oInternals, cons(symbol, symbolTable));
  releaseSymbolTableLock();
  return symbol;
}

obj quote(obj o) { return slotlessObject(oQuote, o); }
obj unquote(obj q) { return hiddenEntity(q); }
int isQuote(obj o) { return -1; } // FIXME: How do we test for this without adding a primitive type?

obj  codeTarget(obj c)   { return idx(hiddenEntity(c), 0); }
obj  codeSelector(obj c) { return idx(hiddenEntity(c), 1); }
pair codeArgs(obj c)     { return idx(hiddenEntity(c), 2); }
obj setCodeTarget(obj c, obj t) { return setIdx(hiddenEntity(c), 0, t); }
int isCode(obj c) { return vectorLength(hiddenEntity(c)) == 3; } // FIXME: Find a better test.

obj message(obj target, obj selector, vector args) {
  return slotlessObject(oCode, newVector(3, target, selector, args));
}
obj setMessageTarget(obj message, obj target) {
  setIdx(hiddenEntity(message), 0, target);
  return message;
}
obj expressionSequence(vector exprs) {
  return message(oMethodBody, sSubexpressions_, exprs);
}

obj threadTarget(vector td) { return continuationTarget(threadContinuation(td)); }

void *safeIdx(vector thread, vector v, int i) {
  return i < vectorLength(v) ? idx(v, i) : 0;
}
void *arg(vector thread, int i) {
  return safeIdx(thread, evaluated(threadContinuation(thread)), i + 1);
}
int arity() {
  return vectorLength(evaluated(threadContinuation(currentThread))) - 1;
}

obj integer(int v) {
  obj i = emptyBignum();
  mpz_init_set_si(bignumData(i), v);
  return i;
}
int integerValue(obj i) {
  return mpz_get_si(bignumData(i));
}

obj *filenameToInclude;
promise *promiseOfInclusion;

// This should never be called, as no primitive object should ever actually be sent a message.
void prototypePrimitiveHiddenValue() {
  die("The prototype primitive's code was executed.");
}

void *loadStream(FILE *, obj, obj);

#define invokeBlock(b) do { \
  continuation c = origin(threadContinuation(currentThread)); \
  setSubexpressionContinuation(currentThread, \
                               c, \
                               env(c), \
                               dynamicEnv(c), \
                               (b), \
                               sAppliedTo_, \
                               newVector(1, vectorObject(emptyVector))); \
  gotoNext; \
} while(0)
#define checkWithoutWaiting(c_value, c_predicate) \
  ({ obj c_newValue = (c_value); \
     for (;;) { \
       if (c_newValue == oNull) { \
         obj e = slotlessObject(oBadTypeException, 0); \
         addCanonSlot(e, sSelector, selector(threadContinuation(currentThread))); \
         raise(e); \
       } \
       if ((c_predicate)(c_newValue)) break; \
       c_newValue = proto(c_newValue); \
     } \
     c_newValue; })
#define check(c_value, c_predicate) (checkWithoutWaiting(waitFor(c_value), (c_predicate)))
#define retarget(r_predicate) (target = checkWithoutWaiting(target, (r_predicate)))
#define safeIntegerValue(siv_i) (integerValue(check((siv_i), isInteger)))
#define safeStringValue(ssv_s) (stringData(check((ssv_s), isString)))
#define safeVector(sv_v) (vectorObjectVector(check((sv_v), isVectorObject)))
#define valueReturn(vr_v) messageReturn(vr_v)
#define normalReturn valueReturn(continuationTarget(threadContinuation(currentThread)))
#define arg(a_i) (arg(currentThread, (a_i)) ?: ({ raise(eMissingArgument); (void *)0; }))
#define resend(r_args) do { \
  continuation r_c = threadContinuation(currentThread); \
  obj r_a = (r_args); \
  setContinuation(currentThread, \
                  newContinuation(origin(r_c), \
                                  selector(r_c), \
                                  r_a, \
                                  r_a, \
                                  env(r_c), \
                                  dynamicEnv(r_c))); \
  tailcall(dispatch); \
} while (0)

// Will undef the "gotoNext" macro.
#include "objects.c"

#undef resend
#undef arg
#undef normalReturn
#undef valueReturn
#undef safeVector
#undef safeStringValue
#undef safeIntegerValue
#undef retarget
#undef check

obj appendSymbols(pair symbols) {
  obj s = string("");
  for (; !empty(symbols); symbols = cdr(symbols))
    s = appendStrings(s, car(symbols));
  setProto(s, oSymbol);
  return intern(s);
}

obj symbol(const char *s) {
  obj o = string(s);
  setProto(o, oSymbol);
  return intern(o);
}

void setupInterpreter() {
  initializeHeap();
  initializeMainThread();
  setCurrentThread(garbageCollectorRoot = createGarbageCollectorRoot(oObject));
  initializeObjects();
  initializePrototypeTags();
  intern(oSymbol);
}

void *loadStream(FILE *f, obj staticEnv, obj dynamicEnv) {
  void *scanner = beginParsing(f);
  obj parserOutput, lastValue = oNull;
  while ((parserOutput = parse(scanner)) != oEndOfFile) {
    if (parserOutput == eSyntaxError) {
      lastValue = eSyntaxError;
      break;
    }
    promise p = newPromise();
    newThread(p, staticEnv, dynamicEnv, parserOutput, sIdentity, emptyVector);
    lastValue = waitFor(p);
  }
  endParsing(scanner);
  fclose(f);
  return lastValue;
}

// Currently used only by main.c for loading command-line arguments.
void *loadFile(const char *filename, obj staticEnv, obj dynamicEnv) {
  FILE *f = fopen(filename, "r");
  if (!f) die("Could not open \"%s\" for input.", filename);
  void *lastValue = loadStream(f, staticEnv, dynamicEnv);
  if (lastValue == eSyntaxError) die("Syntax error.");
  return lastValue;
}

